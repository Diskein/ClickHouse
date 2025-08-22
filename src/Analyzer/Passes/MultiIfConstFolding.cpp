#include <optional>
#include <Analyzer/IQueryTreeNode.h>
#include <Analyzer/Passes/MultiIfConstFolding.h>

#include <Analyzer/ColumnNode.h>
#include <Analyzer/FunctionNode.h>
#include <Analyzer/InDepthQueryTreeVisitor.h>
#include <Analyzer/QueryNode.h>
#include <Analyzer/Utils.h>
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

        enterMultiIf(node);
    }

    void enterMultiIf(QueryTreeNodePtr & node)
    {
        auto * function_node = node->as<FunctionNode>();
        if (!function_node || function_node->getFunctionName() != "multiIf")
            return;

        auto & multi_if_function_arguments = function_node->getArguments().getNodes();

        if ((multi_if_function_arguments.size() % 2 == 0) || multi_if_function_arguments.size() < 3)
            return;

        size_t condition_argument_index = 0;
        size_t result_argument_index = 1;

        for (; result_argument_index < multi_if_function_arguments.size(); condition_argument_index += 2, result_argument_index += 2)
        {
            auto condition_node = multi_if_function_arguments[condition_argument_index];
            auto constant_condition = tryGetConstConditionFromColumn(condition_node);

            if (!constant_condition.has_value())
                return;

            if (!*constant_condition)
                continue;

            auto argument_node = multi_if_function_arguments[result_argument_index];

            if (!node->getResultType()->equals(*argument_node->getResultType()))
                return;

            node = argument_node;
            return;
        }

        auto result_node = multi_if_function_arguments.back();
        if (!node->getResultType()->equals(*result_node->getResultType()))
            node = std::move(result_node);
    }

private:
    std::optional<bool> tryGetConstConditionFromColumn(QueryTreeNodePtr node)
    {
        if (node->getNodeType() != QueryTreeNodeType::COLUMN)
            return {};

        const auto * column_node = node->as<ColumnNode>();

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

        if (!column_projection_node)
            return {};

        return tryExtractConstantFromConditionNode(column_projection_node);
    }
};

}

void MultiIfConstFoldingPass::run(QueryTreeNodePtr & query_tree_node, ContextPtr context)
{
    MultiIfConstFoldingVisitor visitor(std::move(context));
    visitor.visit(query_tree_node);
}

}
