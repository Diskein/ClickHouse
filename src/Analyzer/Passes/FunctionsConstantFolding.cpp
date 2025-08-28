#include <Analyzer/Passes/FunctionsConstantFolding.h>

#include <Analyzer/AggregationUtils.h>
#include <Analyzer/ColumnNode.h>
#include <Analyzer/ConstantNode.h>
#include <Analyzer/FunctionNode.h>
#include <Analyzer/HashUtils.h>
#include <Analyzer/IQueryTreeNode.h>
#include <Analyzer/InDepthQueryTreeVisitor.h>
#include <Analyzer/Passes/MultiIfConstFolding.h>
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

class FunctionsConstantFoldingVisitor : public InDepthQueryTreeVisitorWithContext<FunctionsConstantFoldingVisitor>
{
public:
    using Base = InDepthQueryTreeVisitorWithContext<FunctionsConstantFoldingVisitor>;
    using Base::Base;

    void enterImpl(QueryTreeNodePtr & node)
    {
        auto * function_node = node->as<FunctionNode>();

        if (!function_node || !function_node->isOrdinaryFunction())
            return;

        auto function = FunctionFactory::instance().get(function_node->getFunctionName(), getContext());

        if (!function)
            return;

        bool all_arguments_constants = false;
        ColumnsWithTypeAndName arguments_columns;
        for (const auto & argument_node : function_node->getArguments().getNodes())
        {
            auto [argument_column, is_const] = getArgumentColumn(argument_node);

            // TODO: hack for experiments, need to learn how to resolve the type
            if (!argument_column.type)
                return;


            all_arguments_constants &= is_const;
            arguments_columns.emplace_back(std::move(argument_column));
        }

        auto function_base = function->build(arguments_columns);

        if (!function_base->isSuitableForConstantFolding())
            return;

        // TODO: implement randConstant
        auto result_type = function_base->getResultType();
        auto executable_function = function_base->prepare(arguments_columns);

        ColumnPtr column;

        if (all_arguments_constants)
        {
            size_t num_rows = 1;
            if (!arguments_columns.empty())
                num_rows = arguments_columns.front().column->size();
            column = executable_function->execute(arguments_columns, result_type, num_rows, true);
        }
        else
        {
            column = function_base->getConstantResultForNonConstArguments(arguments_columns, result_type);
        }

        if (column && column->getDataType() != result_type->getColumnType())
            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "Unexpected return type from {}. Expected {}. Got {}",
                function->getName(),
                result_type->getColumnType(),
                column->getDataType());

        ConstantNodePtr constant_node;

        /** Do not perform constant folding if there are aggregate or arrayJoin functions inside function.
              * Example: SELECT toTypeName(sum(number)) FROM numbers(10);
              */
        if (column && isColumnConst(*column) && !typeid_cast<const ColumnConst *>(column.get())->getDataColumn().isDummy()
            && !hasAggregateFunctionNodes(node) && !hasFunctionNode(node, "arrayJoin") &&
            /// Sanity check: do not convert large columns to constants
            column->byteSize() < 1_MiB)
        {
            /// Replace function node with result constant node
            constant_node = std::make_shared<ConstantNode>(ConstantValue{std::move(column), std::move(result_type)}, node);
        }

        if (constant_node)
            node = std::move(constant_node);
    }

private:
    std::pair<ColumnWithTypeAndName, bool> getArgumentColumn(QueryTreeNodePtr argument_node)
    {
        if (auto constant_node = tryGetConstNode(argument_node))
        {
            const auto * constant = constant_node->as<ConstantNode>();
            return {{constant->getColumn(), constant->getResultType(), constant->getValueNameAndType().first}, true};
        }
        else if (auto * column_node = argument_node->as<ColumnNode>())
        {
            ColumnWithTypeAndName res;
            res.name = column_node->getColumnName();
            res.type = column_node->getColumnType();
            return {res, false};
        }

        return {{}, false};
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

class GatherQueryNodesVisitor : public ConstInDepthQueryTreeVisitor<GatherQueryNodesVisitor>
{
public:
    void visitImpl(const QueryTreeNodePtr & node)
    {
        if (node->getNodeType() == QueryTreeNodeType::QUERY)
            query_nodes.emplace_back(node);
    }

    std::vector<QueryTreeNodePtr> query_nodes;
};

}

void FunctionsConstantFoldingPass::run(QueryTreeNodePtr & query_tree_node, ContextPtr context)
{
    GatherQueryNodesVisitor gather_query_nodes;
    gather_query_nodes.visit(query_tree_node);

    for (auto query_node : gather_query_nodes.query_nodes)
    {
        FunctionsConstantFoldingVisitor visitor(std::move(context));
        visitor.visit(query_tree_node);
    }
}

}
