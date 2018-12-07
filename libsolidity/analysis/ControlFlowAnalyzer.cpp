/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <libsolidity/analysis/ControlFlowAnalyzer.h>
#include <liblangutil/SourceLocation.h>

using namespace std;
using namespace langutil;
using namespace dev::solidity;

bool ControlFlowAnalyzer::analyze(ASTNode const& _astRoot)
{
	_astRoot.accept(*this);
	return Error::containsOnlyWarnings(m_errorReporter.errors());
}

bool ControlFlowAnalyzer::visit(FunctionDefinition const& _function)
{
	if (_function.isImplemented())
	{
		auto const& functionFlow = m_cfg.functionFlow(_function);
		checkUnassignedStorageReturnValues(_function, functionFlow.entry, functionFlow.exit);
	}
	return false;
}

void ControlFlowAnalyzer::checkUnassignedStorageReturnValues(
	FunctionDefinition const&,// _function,
	CFGNode const* _functionEntry,
	CFGNode const* _functionExit
) const
{
	map<CFGNode const*, set<VariableDeclaration const*>> unassigned;
	map<CFGNode const*, set<VariableOccurrence const*>> unassignedAccess;
	stack<CFGNode const*> nodesToTraverse;
	nodesToTraverse.push(_functionEntry);

	// walk all paths from entry with maximal set of unassigned return values
	while (!nodesToTraverse.empty())
	{
		auto node = nodesToTraverse.top();
		nodesToTraverse.pop();

		auto& unassignedAtNode = unassigned[node];

		for (auto const& variableOccurrence: node->block.variableOccurrences)
		{
			switch (variableOccurrence.kind())
			{
				case VariableOccurrence::Kind::Assignment:
					unassignedAtNode.erase(variableOccurrence.declaration());
					break;
				case VariableOccurrence::Kind::InlineAssembly:
					// Kill all return values referenced in inline assembly.
					// So far a reference is enough, but we might want to check whether
					// there actually was an assignment at some point.
					unassignedAtNode.erase(variableOccurrence.declaration());
					break;
				case VariableOccurrence::Kind::Access:
					if (unassignedAtNode.count(variableOccurrence.declaration()))
					{
						if (variableOccurrence.declaration()->type()->dataStoredIn(DataLocation::Storage))
							unassignedAccess[node].insert(&variableOccurrence);
					}
					break;
				case VariableOccurrence::Kind::Declaration:
					unassignedAtNode.insert(variableOccurrence.declaration());
					break;
			}
		}

		for (auto const& exit: node->exits)
		{
			bool needToTraverse = false;
			{
				auto& unassignedAtExit = unassigned[exit];
				auto oldSize = unassignedAtExit.size();
				unassignedAtExit.insert(unassignedAtNode.begin(), unassignedAtNode.end());
				if (unassignedAtExit.size() > oldSize)
					needToTraverse = true;
			}
			{
				auto &unassignedAccessAtExit = unassignedAccess[exit];
				auto oldSize = unassignedAccessAtExit.size();
				unassignedAccessAtExit += unassignedAccess[node];
				if (unassignedAccessAtExit.size() > oldSize)
					needToTraverse = true;
			}
			if (needToTraverse)
				nodesToTraverse.push(exit);
		}
	}

	if (!unassignedAccess[_functionExit].empty())
	{
		vector<VariableOccurrence const *> unassignedAccessOrdered(
			unassignedAccess[_functionExit].begin(),
			unassignedAccess[_functionExit].end()
		);
		sort(
			unassignedAccessOrdered.begin(),
			unassignedAccessOrdered.end(),
			[](VariableOccurrence const *lhs, VariableOccurrence const *rhs) -> bool
			{
				if (lhs->occurrence()->id() < rhs->occurrence()->id()) return true;
				if (rhs->occurrence()->id() < lhs->occurrence()->id()) return false;
				if (lhs->declaration()->id() < rhs->declaration()->id()) return true;
				if (rhs->declaration()->id() < lhs->declaration()->id()) return false;
				return static_cast<size_t>(lhs->kind()) < static_cast<size_t>(rhs->kind());
			}
		);
		for (auto const* variableOccurrence: unassignedAccessOrdered)
		{
			m_errorReporter.typeError(
				variableOccurrence->occurrence()->location(),
				"This variable is of storage pointer type and is accessed without prior assignment."
			);
		}
	}
}
