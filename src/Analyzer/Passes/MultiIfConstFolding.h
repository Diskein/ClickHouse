#pragma once

#include <Analyzer/IQueryTreePass.h>

namespace DB
{

/** Convert `multiIf` with constant-resolved branch into this branch argument.
  *
  * Example: SELECT multiIf(1, x, y);
  * Result: SELECT x;
  *
  *
  * Example: SELECT multiIf(atype, IPv4NumToString(reinterpretAsUInt32(reverse(s))), NOT atype, IPv6NumToString(toFixedString(s, 16)), s) FROM (SELECT 99 as atype, 'abcdefghijklmnopq' as s);
  * Result: SELECT IPv4NumToString(reinterpretAsUInt32(reverse(s)))
  */
class MultiIfConstFoldingPass final : public IQueryTreePass
{
public:
    String getName() override { return "MultiIfConstFolding"; }

    String getDescription() override { return "Optimize multiIf if it has constant resolution."; }

    void run(QueryTreeNodePtr & query_tree_node, ContextPtr context) override;

};

}

