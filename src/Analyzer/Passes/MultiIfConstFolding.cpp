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

private:
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

    std::optional<bool> tryGetConstConditionFromNode(QueryTreeNodePtr node)
    {
        if (node->getNodeType() == QueryTreeNodeType::CONSTANT)
            return tryExtractConstantFromConditionNode(node);
        else if (node->getNodeType() == QueryTreeNodeType::COLUMN)
            return tryGetConstConditionFromColumn(node);
        else
            return {};
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
};

}

void MultiIfConstFoldingPass::run(QueryTreeNodePtr & query_tree_node, ContextPtr context)
{
    MultiIfConstFoldingVisitor visitor(std::move(context));
    visitor.visit(query_tree_node);
}

}
