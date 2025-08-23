#include <optional>
#include <Analyzer/ConstantNode.h>
#include <Analyzer/IQueryTreeNode.h>
#include <Analyzer/Passes/MultiIfConstFolding.h>

#include <Analyzer/ColumnNode.h>
#include <Analyzer/FunctionNode.h>
#include <Analyzer/InDepthQueryTreeVisitor.h>
#include <Analyzer/QueryNode.h>
#include <Analyzer/Utils.h>
#include <Core/ColumnWithTypeAndName.h>
#include <Core/ColumnsWithTypeAndName.h>
#include <Core/Settings.h>
#include <Functions/FunctionFactory.h>
#include <Functions/if.h>

namespace DB
{

namespace
{

// TODO: without constant folding of boolean operation it is not quite useful
class MultiIfConstFoldingVisitor : public InDepthQueryTreeVisitorWithContext<MultiIfConstFoldingVisitor>
{
public:
    using Base = InDepthQueryTreeVisitorWithContext<MultiIfConstFoldingVisitor>;
    using Base::Base;

    explicit MultiIfConstFoldingVisitor(ContextPtr context)
        : Base(std::move(context))
    {
    }

    void enterImpl(QueryTreeNodePtr & node)
    {
        auto * function_node = node->as<FunctionNode>();

        if (!function_node)
            return;

        if (function_node->getFunctionName() == "multiIf")
            enterMultiIf(node);
        else if (function_node->getFunctionName() == "if")
            enterIf(node);
    }

    void enterIf(QueryTreeNodePtr & node)
    {
        auto * function_node = node->as<FunctionNode>();
        if (!function_node || function_node->getFunctionName() != "if" || function_node->getArguments().getNodes().size() != 3)
            return;

        const auto & arguments_nodes = function_node->getArguments().getNodes();

        auto constant_condition = tryGetConstConditionFromNode(arguments_nodes[0]);
        if (!constant_condition.has_value())
            return;

        node = *constant_condition ? arguments_nodes[1] : arguments_nodes[2];
    }

    void enterMultiIf(QueryTreeNodePtr & node)
    {
        auto * function_node = node->as<FunctionNode>();
        if (!function_node || function_node->getFunctionName() != "multiIf")
            return;

        const auto & multi_if_function_arguments = function_node->getArguments().getNodes();

        if ((multi_if_function_arguments.size() % 2 == 0) || multi_if_function_arguments.size() < 3)
            return;

        size_t condition_argument_index = 0;
        size_t result_argument_index = 1;

        for (; result_argument_index < multi_if_function_arguments.size(); condition_argument_index += 2, result_argument_index += 2)
        {
            auto condition_node = multi_if_function_arguments[condition_argument_index];
            auto constant_condition = tryGetConstConditionFromNode(condition_node);

            if (!constant_condition.has_value())
                return;

            if (!*constant_condition)
                continue;

            node = multi_if_function_arguments[result_argument_index];
            return;
        }

        node = multi_if_function_arguments.back();
    }

private:
    std::optional<bool> tryGetConstConditionFromNode(QueryTreeNodePtr node)
    {
        if (node->getNodeType() == QueryTreeNodeType::CONSTANT)
            return tryExtractConstantFromConditionNode(node);
        else if (node->getNodeType() == QueryTreeNodeType::COLUMN)
            return tryGetConstConditionFromColumn(node);
        else if (node->getNodeType() == QueryTreeNodeType::FUNCTION)
            return tryGetConstConditionFromFunction(node);
        else
            return {};
    }
    std::optional<bool> tryGetConstConditionFromFunction(QueryTreeNodePtr node)
    {
        static const std::unordered_set<std::string_view> allowed_functions{
            "equals",
            "notEquals",
            "less",
            "lessOrEquals",
            "greater",
            "greaterOrEquals",
        };

        const auto * function_node = node->as<FunctionNode>();

        if (!function_node || (!allowed_functions.contains(function_node->getFunctionName()))
            || function_node->getArguments().getNodes().size() != 2)
            return {};

        const auto & arguments_nodes = function_node->getArguments().getNodes();

        auto lhs_node = tryGetConstNode(arguments_nodes[0]);
        auto rhs_node = tryGetConstNode(arguments_nodes[1]);

        if (!lhs_node || !rhs_node)
            return {};

        auto * lhs_constant_node = lhs_node->as<ConstantNode>();
        auto * rhs_constant_node = rhs_node->as<ConstantNode>();

        chassert(lhs_constant_node);
        chassert(rhs_constant_node);

        ColumnsWithTypeAndName const_arguments = {
            {lhs_constant_node->getColumn(),
             lhs_constant_node->getValueNameAndType().second,
             lhs_constant_node->getValueNameAndType().first},
            {rhs_constant_node->getColumn(),
             rhs_constant_node->getValueNameAndType().second,
             rhs_constant_node->getValueNameAndType().first},
        };

        auto equal_function = FunctionFactory::instance().get(function_node->getFunctionName(), getContext())->build(const_arguments);
        auto res_column
            = equal_function->execute(const_arguments, function_node->getResultType(), /* input_rows_count */ 1, /* dry_run */ false);

        if (!res_column || !res_column->isNumeric())
            return {};

        return res_column->getBool(0);
    }
    std::optional<bool> tryGetConstConditionFromColumn(QueryTreeNodePtr node)
    {
        return tryExtractConstantFromConditionNode(tryGetConstNodeFromColumn(node));
    }
    QueryTreeNodePtr tryGetConstNodeFromColumn(QueryTreeNodePtr node)
    {
        auto * column_node = node->as<ColumnNode>();

        if (!column_node)
            return {};

        auto source_node = getExpressionSource(node);

        if (!source_node)
            return {};

        const auto * source_query_node = source_node->as<QueryNode>();

        if (!source_query_node)
            return {};

        const auto & projections = source_query_node->getProjection().getNodes();
        const auto & projections_columns = source_query_node->getProjectionColumns();

        QueryTreeNodePtr column_projection_node;

        for (size_t i = 0; i < projections_columns.size(); ++i)
        {
            if (projections_columns[i].name != column_node->getColumnName())
                continue;

            column_projection_node = projections.at(i);
        }
        return column_projection_node;
    }
    QueryTreeNodePtr tryGetConstNode(QueryTreeNodePtr node)
    {
        if (node->getNodeType() == QueryTreeNodeType::CONSTANT)
            return node;
        else if (node->getNodeType() == QueryTreeNodeType::COLUMN)
            return tryGetConstNodeFromColumn(node);
        else
            return nullptr;
    }
};

}

void MultiIfConstFoldingPass::run(QueryTreeNodePtr & query_tree_node, ContextPtr context)
{
    MultiIfConstFoldingVisitor visitor(std::move(context));
    visitor.visit(query_tree_node);
}

}
