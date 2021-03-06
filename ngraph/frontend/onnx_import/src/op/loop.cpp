//*****************************************************************************
// Copyright 2017-2020 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************
#include "loop.hpp"

#include <iterator>
#include <memory>

#include "ngraph/function.hpp"
#include "ngraph/log.hpp"
#include "ngraph/op/util/op_types.hpp"
#include "onnx_import/core/graph.hpp"
#include "onnx_import/core/null_node.hpp"
#include "onnx_import/default_opset.hpp"
#include "onnx_import/exceptions.hpp"
#include "onnx_import/utils/reshape.hpp"

namespace ngraph
{
    namespace onnx_import
    {
        namespace op
        {
            namespace set_1
            {
                namespace
                {
                    /// \brief      Check if termination condition is true during all Loop
                    ///             iterations.
                    ///             It allows to replace termination condition body output with
                    ///             Constant.
                    ///             As a result ngraph Loop shape inference is able to handle more
                    ///             cases.
                    ///
                    /// \param[in]  body_out_cond   Termination loop condition input of the body of
                    ///                             the Loop (value updated during Loop iterations).
                    ///
                    /// \return true if termination condition is true and it cannot be changed
                    ///         during Loop iterations, false otherwise.
                    bool is_termination_condition_always_true(
                        const Output<ngraph::Node>& body_out_cond)
                    {
                        // If body termination condition input matches Indentity op pattern the has
                        // value of loop_cond - true
                        // Identity op for boolean value is represented by LogicalOr op whose second
                        // input is always false
                        if (is_type<default_opset::LogicalOr>(body_out_cond.get_node_shared_ptr()))
                        {
                            const auto second_input = body_out_cond.get_node_shared_ptr()
                                                          ->input_value(1)
                                                          .get_node_shared_ptr();
                            if (ngraph::op::is_constant(second_input) &&
                                second_input->get_element_type() == element::boolean &&
                                as_type_ptr<default_opset::Constant>(second_input)
                                        ->cast_vector<bool>()
                                        .at(0) == false)
                            {
                                return true;
                            }
                        }
                        return false;
                    }
                }

                OutputVector loop(const Node& node)
                {
                    const auto& ng_inputs = node.get_ng_inputs();

                    const OutputVector loop_carried_dependencies{std::next(ng_inputs.begin(), 2),
                                                                 ng_inputs.end()};

                    const Subgraph& body_graph{node.get_attribute_value<Subgraph>("body")};
                    auto body_outputs = body_graph.get_ng_outputs();
                    const auto& body_inputs = body_graph.get_ng_parameters();

                    // optional inputs
                    Output<ngraph::Node> trip_count;
                    if (ngraph::op::is_null(ng_inputs.at(0))) // trip count skipped
                    {
                        // -1 means infinite Loop
                        trip_count = ngraph::op::Constant::create(ngraph::element::i64, {1}, {-1});
                    }
                    else
                    {
                        trip_count = ng_inputs.at(0);
                    }

                    Output<ngraph::Node>
                        termination_cond; // true means that first interation should be run
                    if (ngraph::op::is_null(
                            ng_inputs.at(1).get_node_shared_ptr())) // termination condition skipped
                    {
                        termination_cond =
                            ngraph::op::Constant::create(ngraph::element::boolean, {1}, {true});
                    }
                    else if (ngraph::op::is_constant(ng_inputs.at(1).get_node_shared_ptr()))
                    {
                        const auto term_cond_const = as_type_ptr<default_opset::Constant>(
                            ng_inputs.at(1).get_node_shared_ptr());
                        if (term_cond_const->cast_vector<bool>()[0])
                        {
                            termination_cond =
                                ngraph::op::Constant::create(ngraph::element::boolean, {1}, {true});
                        }
                        else
                        {
                            // no iteration is performed so initial values are returned
                            OutputVector node_outputs;
                            // final values
                            for (const auto& dep : loop_carried_dependencies)
                            {
                                node_outputs.push_back(dep);
                            }
                            // scan outputs
                            for (const auto& dep : loop_carried_dependencies)
                            {
                                node_outputs.push_back(dep);
                            }
                            return node_outputs;
                        }
                    }
                    else
                    {
                        // It is temporary solution caused by not supported termination_cond==false
                        // (for not consant case) by nG Loop
                        termination_cond =
                            ngraph::op::Constant::create(ngraph::element::boolean, {1}, {true});
                    }

                    const int64_t concat_axis = 0;
                    const auto concat_axis_const =
                        ngraph::op::Constant::create(ngraph::element::i64, {1}, {concat_axis});
                    // provide scalar handing for scan outputs
                    for (int i = loop_carried_dependencies.size() + 1; i < body_outputs.size(); ++i)
                    {
                        auto body_output_shape = body_outputs[i].get_partial_shape();
                        if (body_output_shape.is_static() &&
                            ngraph::is_scalar(body_output_shape.to_shape()))
                        {
                            body_outputs[i] = std::make_shared<default_opset::Unsqueeze>(
                                body_outputs[i], concat_axis_const);
                        }
                    }

                    const auto& body_loop_out_cond = body_outputs.at(0).get_node_shared_ptr();
                    // optimization allow to improve nG Loop shape inference
                    if (is_termination_condition_always_true(body_loop_out_cond))
                    {
                        body_outputs[0] =
                            ngraph::op::Constant::create(ngraph::element::boolean, {1}, {true});
                    }
                    else
                    {
                        NGRAPH_WARN
                            << "ONNX Loop: No identity or constant termination condition output "
                            << "body is not supported in current version\n";
                        // TODO: It should be removed after introduction fix to nG Loop
                    }

                    CHECK_VALID_NODE(node,
                                     body_inputs.size() >= loop_carried_dependencies.size() + 2,
                                     "The provided loop body graph inputs size (",
                                     body_inputs.size(),
                                     "), is not greater than the sum of loop carried dependencies "
                                     "and two mandatory"
                                     " inputs (",
                                     loop_carried_dependencies.size() + 2,
                                     ")");

                    CHECK_VALID_NODE(node,
                                     body_outputs.size() >= loop_carried_dependencies.size() + 1,
                                     "The provided loop body graph outputs size (",
                                     body_outputs.size(),
                                     ") is not greater than number of outputs. Required at least: ",
                                     loop_carried_dependencies.size() + 1);

                    ParameterVector body_params(body_inputs.begin() + 2, body_inputs.end());
                    body_params.emplace(body_params.begin(),
                                        body_inputs[0]); // termination condition body input
                    const auto body = std::make_shared<ngraph::Function>(body_outputs, body_params);
                    auto loop = std::make_shared<default_opset::Loop>(trip_count, termination_cond);
                    ngraph::opset5::Loop::SpecialBodyPorts spec_ports{0, 0};
                    loop->set_special_body_ports(spec_ports);
                    loop->set_function(body);

                    // Setting up other Loop body inputs.
                    // body_inputs[0] is iteration number, body_inputs[1] is termination condition
                    auto body_inputs_it = std::next(body_inputs.begin(), 2);
                    // body_outputs[0] is termination condition output
                    auto body_outputs_it = std::next(body_outputs.begin(), 1);

                    // Set-up loop carried dependencies and final output values
                    OutputVector final_values;
                    for (const auto& dep : loop_carried_dependencies)
                    {
                        loop->set_merged_input(*body_inputs_it++, dep, *body_outputs_it);
                        final_values.push_back(loop->get_iter_value(*body_outputs_it++, -1));
                    }

                    // Set-up scan outputs
                    OutputVector scan_outputs;
                    for (; body_outputs_it != body_outputs.end(); body_outputs_it++)
                    {
                        // start=0, stride=1, part_size=1, end=-1, axis=0
                        scan_outputs.push_back(loop->get_concatenated_slices(
                            *body_outputs_it, 0, 1, 1, -1, concat_axis));
                    }

                    OutputVector node_outputs;
                    for (const auto& v : final_values)
                    {
                        node_outputs.push_back(v);
                    }
                    for (const auto& v : scan_outputs)
                    {
                        node_outputs.push_back(v);
                    }
                    return node_outputs;
                }
            } // namespace set_1
        }     // namespace op
    }         // namespace onnx_import
} // namespace ngraph
