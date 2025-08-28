#pragma once

#include <Analyzer/IQueryTreePass.h>

namespace DB
{

class FunctionsConstantFoldingPass final : public IQueryTreePass
{
public:
    String getName() override { return "FunctionsConstantFoldingPass"; }

    String getDescription() override { return "constant folding for functions which is suitable for that"; }

    void run(QueryTreeNodePtr & query_tree_node, ContextPtr context) override;

};

}

