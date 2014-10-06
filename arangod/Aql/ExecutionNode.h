////////////////////////////////////////////////////////////////////////////////
/// @brief Infrastructure for ExecutionPlans
///
/// @file arangod/Aql/ExecutionNode.h
///
/// DISCLAIMER
///
/// Copyright 2010-2014 triagens GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Max Neunhoeffer
/// @author Copyright 2014, triagens GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_AQL_EXECUTION_NODE_H
#define ARANGODB_AQL_EXECUTION_NODE_H 1

#include <Basics/Common.h>

#include <Basics/JsonHelper.h>
#include <VocBase/voc-types.h>
#include <VocBase/vocbase.h>

#include "Aql/Collection.h"
#include "Aql/Expression.h"
#include "Aql/Index.h"
#include "Aql/ModificationOptions.h"
#include "Aql/Query.h"
#include "Aql/RangeInfo.h"
#include "Aql/Types.h"
#include "Aql/Variable.h"
#include "Aql/WalkerWorker.h"
#include "Aql/Ast.h"

#include "lib/Basics/json-utilities.h"

namespace triagens {
  namespace basics {
    class StringBuffer;
  }

  namespace aql {

    class Ast;
    class ExecutionBlock;
    class ExecutionPlan;
    class RedundantCalculationsReplacer;

////////////////////////////////////////////////////////////////////////////////
/// @brief pairs, consisting of variable and sort direction
/// (true = ascending | false = descending)
////////////////////////////////////////////////////////////////////////////////

    typedef std::vector<std::pair<Variable const*, bool>> SortElementVector;

////////////////////////////////////////////////////////////////////////////////
/// @brief class ExecutionNode, abstract base class of all execution Nodes
////////////////////////////////////////////////////////////////////////////////

    class ExecutionNode {

////////////////////////////////////////////////////////////////////////////////
/// @brief node type
////////////////////////////////////////////////////////////////////////////////

        friend class ExecutionBlock;

      public:

        enum NodeType {
          ILLEGAL                 =  0,
          SINGLETON               =  1, 
          ENUMERATE_COLLECTION    =  2, 
          INDEX_RANGE             =  3,
          ENUMERATE_LIST          =  4, 
          FILTER                  =  5, 
          LIMIT                   =  6, 
          CALCULATION             =  7, 
          SUBQUERY                =  8, 
          SORT                    =  9, 
          AGGREGATE               = 10, 
          SCATTER                 = 11,
          GATHER                  = 12,
          REMOTE                  = 13,
          INSERT                  = 14,
          REMOVE                  = 15,
          REPLACE                 = 16,
          UPDATE                  = 17,
          RETURN                  = 18,
          NORESULTS               = 19,
          DISTRIBUTE              = 20
        };

// -----------------------------------------------------------------------------
// --SECTION--                                        constructors / destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief constructor using an id
////////////////////////////////////////////////////////////////////////////////

        ExecutionNode (ExecutionPlan* plan, size_t id)
          : _id(id), 
            _estimatedCost(0.0), 
            _estimatedCostSet(false),
            _varUsageValid(false),
            _plan(plan),
            _depth(0) {
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief constructor using a JSON struct
////////////////////////////////////////////////////////////////////////////////

        ExecutionNode (ExecutionPlan* plan, triagens::basics::Json const& json);

////////////////////////////////////////////////////////////////////////////////
/// @brief destructor, free dependencies;
////////////////////////////////////////////////////////////////////////////////

        virtual ~ExecutionNode () { 
        }

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------
      
      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief factory from json.
////////////////////////////////////////////////////////////////////////////////

        static ExecutionNode* fromJsonFactory (ExecutionPlan* plan,
                                               triagens::basics::Json const& json);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the node's id
////////////////////////////////////////////////////////////////////////////////

        inline size_t id () const {
          return _id;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the type of the node
////////////////////////////////////////////////////////////////////////////////

        virtual NodeType getType () const = 0;

////////////////////////////////////////////////////////////////////////////////
/// @brief return the type name of the node
////////////////////////////////////////////////////////////////////////////////

        std::string const& getTypeString () const;

////////////////////////////////////////////////////////////////////////////////
/// @brief checks whether we know a type of this kind; throws exception if not.
////////////////////////////////////////////////////////////////////////////////

        static void validateType (int type);

////////////////////////////////////////////////////////////////////////////////
/// @brief add a dependency
////////////////////////////////////////////////////////////////////////////////

        void addDependency (ExecutionNode* ep) {
          _dependencies.push_back(ep);
          ep->_parents.push_back(this);
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief get all dependencies
////////////////////////////////////////////////////////////////////////////////

        std::vector<ExecutionNode*> getDependencies () const {
          return _dependencies;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief get all parents
////////////////////////////////////////////////////////////////////////////////

        std::vector<ExecutionNode*> getParents () const {
          return _parents;
        }


////////////////////////////////////////////////////////////////////////////////
/// @brief inspect one index; only skiplist indices which match attrs in sequence.
/// returns a a qualification how good they match;
///      match->index==nullptr means no match at all.
////////////////////////////////////////////////////////////////////////////////

        enum MatchType {
          FORWARD_MATCH,
          REVERSE_MATCH,
          NOT_COVERED_IDX,
          NOT_COVERED_ATTR,
          NO_MATCH
        };

        struct IndexMatch {
          IndexMatch () 
            : index(nullptr),
              doesMatch(false),
              reverse(false) {
          }

          Index const* index;              // The index concerned; if null, this is a nonmatch.
          std::vector<MatchType> matches;  // qualification of the attrs match quality
          bool doesMatch;                  // do all criteria match?
          bool reverse;                    // reverse index scan required
        };

        typedef std::vector<std::pair<std::string, bool>> IndexMatchVec;

        static IndexMatch CompareIndex (Index const* idx,
                                        IndexMatchVec const& attrs);

////////////////////////////////////////////////////////////////////////////////
/// @brief replace a dependency, returns true if the pointer was found and 
/// replaced, please note that this does not delete oldNode!
////////////////////////////////////////////////////////////////////////////////

        bool replaceDependency (ExecutionNode* oldNode, ExecutionNode* newNode) {
          auto it = _dependencies.begin(); 

          while (it != _dependencies.end()) {
            if (*it == oldNode) {
              *it = newNode;
              try {
                newNode->_parents.push_back(this);
              }
              catch (...) {
                *it = oldNode;  // roll back
                return false;
              }
              try {
                for (auto it2 = oldNode->_parents.begin();
                     it2 != oldNode->_parents.end();
                     ++it2) {
                  if (*it2 == this) {
                    oldNode->_parents.erase(it2);
                    break;
                  }
                }
              }
              catch (...) {
                // If this happens, we ignore that the _parents of oldNode
                // are not set correctly
              }
              return true;
            }
            ++it;
          }
          return false;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief remove a dependency, returns true if the pointer was found and 
/// removed, please note that this does not delete ep!
////////////////////////////////////////////////////////////////////////////////

        bool removeDependency (ExecutionNode* ep) {
          bool ok = false;
          for (auto it = _dependencies.begin();
               it != _dependencies.end();
               ++it) {
            if (*it == ep) {
              try {
                _dependencies.erase(it);
              }
              catch (...) {
                return false;
              }
              ok = true;
              break;
            }
          }
          if (! ok) {
            return false;
          }

          // Now remove us as a parent of the old dependency as well:
          for (auto it = ep->_parents.begin(); 
               it != ep->_parents.end(); 
               ++it) {
            if (*it == this) {
              try {
                ep->_parents.erase(it);
              }
              catch (...) {
              }
              return true;
            }
          }

          return false;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief remove all dependencies for the given node
////////////////////////////////////////////////////////////////////////////////

        void removeDependencies () {
          for (auto x : _dependencies) {
            for (auto it = x->_parents.begin();
                 it != x->_parents.end();
                 ++it) {
              if (*it == this) {
                try {
                  x->_parents.erase(it);
                }
                catch (...) {
                }
                break;
              }
            }
          }
          _dependencies.clear();
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief clone execution Node recursively, this makes the class abstract
////////////////////////////////////////////////////////////////////////////////

        virtual ExecutionNode* clone (ExecutionPlan* plan,
                                      bool withDependencies,
                                      bool withProperties) const = 0;
        
          // make class abstract

////////////////////////////////////////////////////////////////////////////////
/// @brief execution Node clone utility to be called by derives
////////////////////////////////////////////////////////////////////////////////
        void CloneHelper (ExecutionNode *Other,
                          ExecutionPlan* plan,
                          bool withDependencies,
                          bool withProperties) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief helper for cloning, use virtual clone methods for dependencies
////////////////////////////////////////////////////////////////////////////////

        void cloneDependencies (ExecutionPlan* plan,
                                ExecutionNode* theClone,
                                bool withProperties) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief convert to a string, basically for debugging purposes
////////////////////////////////////////////////////////////////////////////////

        virtual void appendAsString (std::string& st, int indent = 0);

////////////////////////////////////////////////////////////////////////////////
/// @brief invalidate the cost estimation for the node and its dependencies
////////////////////////////////////////////////////////////////////////////////
        
        void invalidateCost () {
          _estimatedCostSet = false;
          
          for (auto dep : _dependencies) {
            dep->invalidateCost();
          }
        }
       
////////////////////////////////////////////////////////////////////////////////
/// @brief estimate the cost of the node . . .
////////////////////////////////////////////////////////////////////////////////
        
        double getCost () {
          if (! _estimatedCostSet) {
            _estimatedCost = estimateCost();
            _estimatedCostSet = true;
            TRI_ASSERT(_estimatedCost >= 0.0);
          }
          return _estimatedCost;
        };

        virtual double estimateCost () = 0;
        
        //TODO nodes should try harder to estimate their own cost, i.e. the cost
        //of performing the operation of the node . . .

////////////////////////////////////////////////////////////////////////////////
/// @brief walk a complete execution plan recursively
////////////////////////////////////////////////////////////////////////////////

        bool walk (WalkerWorker<ExecutionNode>* worker);

////////////////////////////////////////////////////////////////////////////////
/// @brief export to JSON, returns an AUTOFREE Json object
////////////////////////////////////////////////////////////////////////////////

        triagens::basics::Json toJson (TRI_memory_zone_t*,
                                       bool) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief toJson
////////////////////////////////////////////////////////////////////////////////

        virtual void toJsonHelper (triagens::basics::Json&,
                                   TRI_memory_zone_t*,
                                   bool) const = 0;

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesUsedHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesUsedHere () const {
          return std::vector<Variable const*>();
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesSetHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesSetHere () const {
          return std::vector<Variable const*>();
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief setVarsUsedLater
////////////////////////////////////////////////////////////////////////////////

        void setVarsUsedLater (std::unordered_set<Variable const*>& v) {
          _varsUsedLater = v;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getVarsUsedLater, this returns the set of variables that will be
/// used later than this node, i.e. in the repeated parents.
////////////////////////////////////////////////////////////////////////////////

        std::unordered_set<Variable const*> const& getVarsUsedLater () const {
          TRI_ASSERT(_varUsageValid);
          return _varsUsedLater;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief setVarsValid
////////////////////////////////////////////////////////////////////////////////

        void setVarsValid (std::unordered_set<Variable const*>& v) {
          _varsValid = v;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getVarsValid, this returns the set of variables that is valid 
/// for items leaving this node, this includes those that will be set here
/// (see getVariablesSetHere).
////////////////////////////////////////////////////////////////////////////////

        std::unordered_set<Variable const*> const& getVarsValid () const {
          TRI_ASSERT(_varUsageValid);
          return _varsValid;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief setVarUsageValid
////////////////////////////////////////////////////////////////////////////////

        void setVarUsageValid () {
          _varUsageValid = true;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief invalidateVarUsage
////////////////////////////////////////////////////////////////////////////////

        void invalidateVarUsage () {
          _varsUsedLater.clear();
          _varsValid.clear();
          _varUsageValid = false;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief can the node throw?
////////////////////////////////////////////////////////////////////////////////

        virtual bool canThrow () {
          return false;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief static analysis, walker class and information collector
////////////////////////////////////////////////////////////////////////////////

        struct VarInfo {
          unsigned int depth;
          RegisterId registerId;

          VarInfo () = delete;
          VarInfo (int depth, RegisterId registerId)
            : depth(depth), registerId(registerId) {
    
            TRI_ASSERT(registerId < MaxRegisterId);
          }
        };

        struct VarOverview : public WalkerWorker<ExecutionNode> {
          // The following are collected for global usage in the ExecutionBlock,
          // although they are stored here in the node:

          // map VariableIds to their depth and registerId:
          std::unordered_map<VariableId, VarInfo> varInfo;

          // number of variables in the frame of the current depth:
          std::vector<RegisterId>                 nrRegsHere;

          // number of variables in this and all outer frames together,
          // the entry with index i here is always the sum of all values
          // in nrRegsHere from index 0 to i (inclusively) and the two
          // have the same length:
          std::vector<RegisterId>                 nrRegs;

          // We collect the subquery nodes to deal with them at the end:
          std::vector<ExecutionNode*>             subQueryNodes;

          // Local for the walk:
          unsigned int depth;
          unsigned int totalNrRegs;

          // This is used to tell all nodes and share a pointer to ourselves
          shared_ptr<VarOverview>* me;

          VarOverview ()
            : depth(0), totalNrRegs(0), me(nullptr) {
            nrRegsHere.push_back(0);
            nrRegs.push_back(0);
          };

          void setSharedPtr (shared_ptr<VarOverview>* shared) {
            me = shared;
          }

          // Copy constructor used for a subquery:
          VarOverview (VarOverview const& v, unsigned int newdepth);
          ~VarOverview () {};

          virtual bool enterSubquery (ExecutionNode*,
                                      ExecutionNode*) {
            return false;  // do not walk into subquery
          }

          virtual void after (ExecutionNode *eb);

          VarOverview* clone(ExecutionPlan* plan);

        };

////////////////////////////////////////////////////////////////////////////////
/// @brief static analysis
////////////////////////////////////////////////////////////////////////////////

        void planRegisters (ExecutionNode* super = nullptr);

////////////////////////////////////////////////////////////////////////////////
/// @brief get varOverview
////////////////////////////////////////////////////////////////////////////////

        VarOverview const* getVarOverview () const {
          return _varOverview.get();
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief get depth
////////////////////////////////////////////////////////////////////////////////

        int getDepth () const {
          return _depth;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief get registers to clear
////////////////////////////////////////////////////////////////////////////////

        std::unordered_set<RegisterId> const& getRegsToClear () const {
          return _regsToClear;
        }

// -----------------------------------------------------------------------------
// --SECTION--                                               protected functions
// -----------------------------------------------------------------------------
      
      protected:

////////////////////////////////////////////////////////////////////////////////
/// @brief factory for (optional) variables from json.
////////////////////////////////////////////////////////////////////////////////

        static Variable* varFromJson (Ast* ast,
                                      triagens::basics::Json const& base,
                                      char const* variableName,
                                      bool optional = false);

////////////////////////////////////////////////////////////////////////////////
/// @brief factory for sort Elements from json.
////////////////////////////////////////////////////////////////////////////////

        static void getSortElements (SortElementVector& elements,
                                     ExecutionPlan* plan,
                                     triagens::basics::Json const& oneNode,
                                     char const* which);

////////////////////////////////////////////////////////////////////////////////
/// @brief toJsonHelper, for a generic node
////////////////////////////////////////////////////////////////////////////////

        triagens::basics::Json toJsonHelperGeneric (triagens::basics::Json&,
                                                    TRI_memory_zone_t*,
                                                    bool) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief set regs to be deleted
////////////////////////////////////////////////////////////////////////////////

        void setRegsToClear (std::unordered_set<RegisterId> const& toClear) {
          _regsToClear = toClear;
        }

// -----------------------------------------------------------------------------
// --SECTION--                                               protected variables
// -----------------------------------------------------------------------------
      
      protected:

////////////////////////////////////////////////////////////////////////////////
/// @brief node id
////////////////////////////////////////////////////////////////////////////////

        size_t const _id;

////////////////////////////////////////////////////////////////////////////////
/// @brief our dependent nodes
////////////////////////////////////////////////////////////////////////////////

        std::vector<ExecutionNode*> _dependencies;

////////////////////////////////////////////////////////////////////////////////
/// @brief our parent nodes
////////////////////////////////////////////////////////////////////////////////

        std::vector<ExecutionNode*> _parents;

////////////////////////////////////////////////////////////////////////////////
/// @brief NodeType to string mapping
////////////////////////////////////////////////////////////////////////////////
        
        static std::unordered_map<int, std::string const> const TypeNames;

////////////////////////////////////////////////////////////////////////////////
/// @brief _estimatedCost = 0 if uninitialised and otherwise stores the result
/// of estimateCost(), the bool indicates if the cost has been set, it starts
/// out as false
////////////////////////////////////////////////////////////////////////////////

        double _estimatedCost;

        bool _estimatedCostSet;

////////////////////////////////////////////////////////////////////////////////
/// @brief _varsUsedLater and _varsValid, the former contains those
/// variables that are still needed further down in the chain. The
/// latter contains the variables that are set from the dependent nodes
/// when an item comes into the current node. Both are only valid if
/// _varUsageValid is true. Use ExecutionPlan::findVarUsage to set
/// this.
////////////////////////////////////////////////////////////////////////////////

        std::unordered_set<Variable const*> _varsUsedLater;

        std::unordered_set<Variable const*> _varsValid;

        bool _varUsageValid;

////////////////////////////////////////////////////////////////////////////////
/// @brief _plan, the ExecutionPlan object
////////////////////////////////////////////////////////////////////////////////

        ExecutionPlan* _plan;

////////////////////////////////////////////////////////////////////////////////
/// @brief info about variables, filled in by planRegisters
////////////////////////////////////////////////////////////////////////////////

        std::shared_ptr<VarOverview> _varOverview;

////////////////////////////////////////////////////////////////////////////////
/// @brief depth of the current frame, will be filled in by planRegisters
////////////////////////////////////////////////////////////////////////////////

        int _depth;

////////////////////////////////////////////////////////////////////////////////
/// @brief the following contains the registers which should be cleared
/// just before this node hands on results. This is computed during
/// the static analysis for each node using the variable usage in the plan.
////////////////////////////////////////////////////////////////////////////////

        std::unordered_set<RegisterId> _regsToClear;
   
      public:
       
////////////////////////////////////////////////////////////////////////////////
/// @brief maximum register id that can be assigned.
/// this is used for assertions
////////////////////////////////////////////////////////////////////////////////
    
        static RegisterId const MaxRegisterId;
    };

// -----------------------------------------------------------------------------
// --SECTION--                                               class SingletonNode
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief class SingletonNode
////////////////////////////////////////////////////////////////////////////////

    class SingletonNode : public ExecutionNode {
      
      friend class ExecutionBlock;
      friend class SingletonBlock;
      
////////////////////////////////////////////////////////////////////////////////
/// @brief constructor with an id
////////////////////////////////////////////////////////////////////////////////

      public:
 
        SingletonNode (ExecutionPlan* plan, size_t id) 
          : ExecutionNode(plan, id) {
        }

        SingletonNode (ExecutionPlan*, triagens::basics::Json const& base);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the type of the node
////////////////////////////////////////////////////////////////////////////////

        NodeType getType () const override {
          return SINGLETON;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief export to JSON
////////////////////////////////////////////////////////////////////////////////

        virtual void toJsonHelper (triagens::basics::Json&,
                                   TRI_memory_zone_t*,
                                   bool) const override;

////////////////////////////////////////////////////////////////////////////////
/// @brief clone ExecutionNode recursively
////////////////////////////////////////////////////////////////////////////////

        virtual ExecutionNode* clone (ExecutionPlan* plan,
                                      bool withDependencies,
                                      bool withProperties) const {
          auto c = new SingletonNode(plan, _id);

          CloneHelper (c, plan, withDependencies, withProperties);

          return static_cast<ExecutionNode*>(c);
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief the cost of a singleton is 1
////////////////////////////////////////////////////////////////////////////////
        
        double estimateCost () {
          return 1;
        }

    };

// -----------------------------------------------------------------------------
// --SECTION--                                     class EnumerateCollectionNode
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief class EnumerateCollectionNode
////////////////////////////////////////////////////////////////////////////////

    class EnumerateCollectionNode : public ExecutionNode {
      friend class ExecutionNode;
      friend class ExecutionBlock;
      friend class EnumerateCollectionBlock;
      
////////////////////////////////////////////////////////////////////////////////
/// @brief constructor with a vocbase and a collection name
////////////////////////////////////////////////////////////////////////////////

      public:

        EnumerateCollectionNode (ExecutionPlan* plan,
                                 size_t id,
                                 TRI_vocbase_t* vocbase, 
                                 Collection* collection,
                                 Variable const* outVariable)
          : ExecutionNode(plan, id), 
            _vocbase(vocbase), 
            _collection(collection),
            _outVariable(outVariable){
          TRI_ASSERT(_vocbase != nullptr);
          TRI_ASSERT(_collection != nullptr);
          TRI_ASSERT(_outVariable != nullptr);
        }

        EnumerateCollectionNode (ExecutionPlan* plan,
                                 triagens::basics::Json const& base);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the type of the node
////////////////////////////////////////////////////////////////////////////////

        NodeType getType () const override {
          return ENUMERATE_COLLECTION;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief export to JSON
////////////////////////////////////////////////////////////////////////////////

        virtual void toJsonHelper (triagens::basics::Json&,
                                   TRI_memory_zone_t*,
                                   bool) const override;

////////////////////////////////////////////////////////////////////////////////
/// @brief clone ExecutionNode recursively
////////////////////////////////////////////////////////////////////////////////

        virtual ExecutionNode* clone (ExecutionPlan* plan,
                                      bool withDependencies,
                                      bool withProperties) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief the cost of an enumerate collection node is a multiple of the cost of
/// its unique dependency
////////////////////////////////////////////////////////////////////////////////
        
        double estimateCost () override { 
          return static_cast<double>(_collection->count()) * _dependencies.at(0)->getCost(); 
          //FIXME improve this estimate . . .
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesSetHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesSetHere () const {
          std::vector<Variable const*> v;
          v.push_back(_outVariable);
          return v;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief get the number of usable fields from the index (according to the
/// attributes passed)
////////////////////////////////////////////////////////////////////////////////

        size_t getUsableFieldsOfIndex (Index const* idx,
                                       std::unordered_set<std::string> const&) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief get vector of indices that has any match in its fields with <attrs> 
////////////////////////////////////////////////////////////////////////////////

        void getIndexesForIndexRangeNode (std::unordered_set<std::string> const& attrs, 
                                          std::vector<Index*>& idxs, 
                                          std::vector<size_t>& prefixes) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief get vector of skiplist indices which match attrs in sequence.
/// @returns a list of indexes with a qualification how good they match 
///    the specified indexes.
////////////////////////////////////////////////////////////////////////////////

        std::vector<IndexMatch> getIndicesOrdered (IndexMatchVec const& attrs) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief return the database
////////////////////////////////////////////////////////////////////////////////

        TRI_vocbase_t* vocbase () const {
          return _vocbase;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the collection
////////////////////////////////////////////////////////////////////////////////

        Collection const* collection () const {
          return _collection;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the out variable
////////////////////////////////////////////////////////////////////////////////

        Variable const* outVariable () const {
          return _outVariable;
        }

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief the database
////////////////////////////////////////////////////////////////////////////////

        TRI_vocbase_t* _vocbase;

////////////////////////////////////////////////////////////////////////////////
/// @brief collection
////////////////////////////////////////////////////////////////////////////////

        Collection* _collection;

////////////////////////////////////////////////////////////////////////////////
/// @brief output variable
////////////////////////////////////////////////////////////////////////////////

        Variable const* _outVariable;

    };

// -----------------------------------------------------------------------------
// --SECTION--                                           class EnumerateListNode
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief class EnumerateListNode
////////////////////////////////////////////////////////////////////////////////

    class EnumerateListNode : public ExecutionNode {
      
      friend class ExecutionNode;
      friend class ExecutionBlock;
      friend class EnumerateListBlock;
      friend class RedundantCalculationsReplacer;

////////////////////////////////////////////////////////////////////////////////
/// @brief constructor
////////////////////////////////////////////////////////////////////////////////

      public:

        EnumerateListNode (ExecutionPlan* plan,
                           size_t id,
                           Variable const* inVariable,
                           Variable const* outVariable) 
          : ExecutionNode(plan, id), 
            _inVariable(inVariable), 
            _outVariable(outVariable) {

          TRI_ASSERT(_inVariable != nullptr);
          TRI_ASSERT(_outVariable != nullptr);
        }
        
        EnumerateListNode (ExecutionPlan*, triagens::basics::Json const& base);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the type of the node
////////////////////////////////////////////////////////////////////////////////

        NodeType getType () const override {
          return ENUMERATE_LIST;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief export to JSON
////////////////////////////////////////////////////////////////////////////////

        virtual void toJsonHelper (triagens::basics::Json&,
                                   TRI_memory_zone_t*,
                                   bool) const override;

////////////////////////////////////////////////////////////////////////////////
/// @brief clone ExecutionNode recursively
////////////////////////////////////////////////////////////////////////////////

        virtual ExecutionNode* clone (ExecutionPlan* plan,
                                      bool withDependencies,
                                      bool withProperties) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief the cost of an enumerate list node is . . . FIXME
////////////////////////////////////////////////////////////////////////////////
        
        double estimateCost () override {
          return 1000 * _dependencies.at(0)->getCost(); 
          //FIXME improve this estimate . . .
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesUsedHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesUsedHere () const {
          std::vector<Variable const*> v;
          v.push_back(_inVariable);
          return v;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesSetHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesSetHere () const {
          std::vector<Variable const*> v;
          v.push_back(_outVariable);
          return v;
        }

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief input variable to read from
////////////////////////////////////////////////////////////////////////////////

        Variable const* _inVariable;

////////////////////////////////////////////////////////////////////////////////
/// @brief output variable to write to
////////////////////////////////////////////////////////////////////////////////

        Variable const* _outVariable;

    };

////////////////////////////////////////////////////////////////////////////////
/// @brief class IndexRangeNode
////////////////////////////////////////////////////////////////////////////////

    class IndexRangeNode: public ExecutionNode {
      
      friend class ExecutionBlock;
      friend class IndexRangeBlock;

////////////////////////////////////////////////////////////////////////////////
/// @brief constructor with a vocbase and a collection name
////////////////////////////////////////////////////////////////////////////////

// _ranges must correspond to a prefix of the fields of the index <index>, i.e.
// _ranges.at(i) is a range of values for idx->_fields._buffer[i]. 

      public:

        IndexRangeNode (ExecutionPlan* plan,
                        size_t id,
                        TRI_vocbase_t* vocbase, 
                        Collection const* collection,
                        Variable const* outVariable,
                        Index const* index, 
                        std::vector<std::vector<RangeInfo>> const& ranges,
                        bool reverse)
          : ExecutionNode(plan, id), 
            _vocbase(vocbase), 
            _collection(collection),
            _outVariable(outVariable),
            _index(index),
            _ranges(ranges),
            _reverse(reverse) {
          TRI_ASSERT(_vocbase != nullptr);
          TRI_ASSERT(_collection != nullptr);
          TRI_ASSERT(_outVariable != nullptr);
          TRI_ASSERT(_index != nullptr);
        }

        IndexRangeNode (ExecutionPlan*, triagens::basics::Json const& base);

        ~IndexRangeNode () {
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the type of the node
////////////////////////////////////////////////////////////////////////////////

        NodeType getType () const override {
          return INDEX_RANGE;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the database
////////////////////////////////////////////////////////////////////////////////
        
        TRI_vocbase_t* vocbase () const {
          return _vocbase;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the collection
////////////////////////////////////////////////////////////////////////////////

        Collection const* collection () const {
          return _collection;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief export to JSON
////////////////////////////////////////////////////////////////////////////////

        virtual void toJsonHelper (triagens::basics::Json&,
                                   TRI_memory_zone_t*,
                                   bool) const override;

////////////////////////////////////////////////////////////////////////////////
/// @brief clone ExecutionNode recursively
////////////////////////////////////////////////////////////////////////////////

        virtual ExecutionNode* clone (ExecutionPlan* plan,
                                      bool withDependencies,
                                      bool withProperties) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesSetHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesSetHere () const {
          std::vector<Variable const*> v;
          v.push_back(_outVariable);
          return v;
        }
        
////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesUsedHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesUsedHere () const;

////////////////////////////////////////////////////////////////////////////////
/// @brief estimateCost
////////////////////////////////////////////////////////////////////////////////

        double estimateCost () override;

////////////////////////////////////////////////////////////////////////////////
/// @brief check whether the pattern matches this nodes index
////////////////////////////////////////////////////////////////////////////////

        IndexMatch MatchesIndex (IndexMatchVec const& pattern) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not a reverse index traversal is used
////////////////////////////////////////////////////////////////////////////////

        void reverse (bool value) {
          _reverse = value;
        }

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief the database
////////////////////////////////////////////////////////////////////////////////

        TRI_vocbase_t* _vocbase;

////////////////////////////////////////////////////////////////////////////////
/// @brief collection
////////////////////////////////////////////////////////////////////////////////

        Collection const* _collection;

////////////////////////////////////////////////////////////////////////////////
/// @brief output variable
////////////////////////////////////////////////////////////////////////////////

        Variable const* _outVariable;

////////////////////////////////////////////////////////////////////////////////
/// @brief the index
////////////////////////////////////////////////////////////////////////////////

        Index const* _index;

////////////////////////////////////////////////////////////////////////////////
/// @brief the range info
////////////////////////////////////////////////////////////////////////////////
        
        std::vector<std::vector<RangeInfo>> _ranges;

////////////////////////////////////////////////////////////////////////////////
/// @brief use a reverse index scan
////////////////////////////////////////////////////////////////////////////////

        bool _reverse;
    };

// -----------------------------------------------------------------------------
// --SECTION--                                                   class LimitNode
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief class LimitNode
////////////////////////////////////////////////////////////////////////////////

    class LimitNode : public ExecutionNode {
      
      friend class ExecutionBlock;
      friend class LimitBlock;

////////////////////////////////////////////////////////////////////////////////
/// @brief constructors for various arguments, always with offset and limit
////////////////////////////////////////////////////////////////////////////////

      public:

        LimitNode (ExecutionPlan* plan,
                   size_t id,
                   size_t offset, 
                   size_t limit) 
          : ExecutionNode(plan, id), 
            _offset(offset), 
            _limit(limit) {
        }

        LimitNode (ExecutionPlan* plan,
                   size_t id,
                   size_t limit) 
          : ExecutionNode(plan, id), 
            _offset(0), 
            _limit(limit) {
        }
        
        LimitNode (ExecutionPlan*, triagens::basics::Json const& base);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the type of the node
////////////////////////////////////////////////////////////////////////////////

        NodeType getType () const override {
          return LIMIT;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief export to JSON
////////////////////////////////////////////////////////////////////////////////

        virtual void toJsonHelper (triagens::basics::Json&,
                                   TRI_memory_zone_t*,
                                   bool) const override;

////////////////////////////////////////////////////////////////////////////////
/// @brief clone ExecutionNode recursively
////////////////////////////////////////////////////////////////////////////////

        virtual ExecutionNode* clone (ExecutionPlan* plan,
                                      bool withDependencies,
                                      bool withProperties) const {
          auto c = new LimitNode(plan, _id, _offset, _limit);

          CloneHelper (c, plan, withDependencies, withProperties);

          return static_cast<ExecutionNode*>(c);
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief the cost of a limit node is the minimum of the _limit, and the cost
/// the dependency . . .
////////////////////////////////////////////////////////////////////////////////
        
        double estimateCost () override {
          return 1.005 * (std::min)(static_cast<double>(_limit), _dependencies.at(0)->getCost());
          //FIXME improve this estimate . . .
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief we need to know the offset and limit
////////////////////////////////////////////////////////////////////////////////

      private:

        size_t _offset;
        size_t _limit;

    };

// -----------------------------------------------------------------------------
// --SECTION--                                             class CalculationNode
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief class CalculationNode
////////////////////////////////////////////////////////////////////////////////

    class CalculationNode : public ExecutionNode {
      
      friend class ExecutionNode;
      friend class ExecutionBlock;
      friend class CalculationBlock;
      friend class RedundantCalculationsReplacer;

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief constructor
////////////////////////////////////////////////////////////////////////////////

        CalculationNode (ExecutionPlan* plan,
                         size_t id,
                         Expression* expr, 
                         Variable const* outVariable)
          : ExecutionNode(plan, id), 
            _outVariable(outVariable),
            _expression(expr) {

          TRI_ASSERT(_expression != nullptr);
          TRI_ASSERT(_outVariable != nullptr);
        }

        CalculationNode (ExecutionPlan*, triagens::basics::Json const& base);

////////////////////////////////////////////////////////////////////////////////
/// @brief destructor
////////////////////////////////////////////////////////////////////////////////

        ~CalculationNode () {
          if (_expression != nullptr) {
            delete _expression;
          }
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the type of the node
////////////////////////////////////////////////////////////////////////////////

        NodeType getType () const override {
          return CALCULATION;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief export to JSON
////////////////////////////////////////////////////////////////////////////////

        virtual void toJsonHelper (triagens::basics::Json&,
                                   TRI_memory_zone_t*,
                                   bool) const override;

////////////////////////////////////////////////////////////////////////////////
/// @brief clone ExecutionNode recursively
////////////////////////////////////////////////////////////////////////////////

        virtual ExecutionNode* clone (ExecutionPlan* plan,
                                      bool withDependencies,
                                      bool withProperties) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief return out variable
////////////////////////////////////////////////////////////////////////////////

        Variable const* outVariable () const {
          return _outVariable;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the expression
////////////////////////////////////////////////////////////////////////////////

        Expression* expression () const {
          return _expression;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief the cost of a calculation node is the cost of the unique dependency
//  times a constant
////////////////////////////////////////////////////////////////////////////////
        
        double estimateCost () override {
          return 2 * _dependencies.at(0)->getCost(); 
          //FIXME improve this estimate . . . 
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesUsedHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesUsedHere () const {
          std::unordered_set<Variable*> vars = _expression->variables();
          std::vector<Variable const*> v;
          for (auto vv : vars) {
            v.push_back(vv);
          }
          return v;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesSetHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesSetHere () const {
          std::vector<Variable const*> v;
          v.push_back(_outVariable);
          return v;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief can the node throw?
////////////////////////////////////////////////////////////////////////////////

        bool canThrow () {
          return _expression->canThrow();
        }

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief output variable to write to
////////////////////////////////////////////////////////////////////////////////

        Variable const* _outVariable;

////////////////////////////////////////////////////////////////////////////////
/// @brief we need to have an expression and where to write the result
////////////////////////////////////////////////////////////////////////////////

        Expression* _expression;

    };

// -----------------------------------------------------------------------------
// --SECTION--                                                class SubqueryNode
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief class SubqueryNode
////////////////////////////////////////////////////////////////////////////////

    class SubqueryNode : public ExecutionNode {
      
      friend class ExecutionNode;
      friend class ExecutionBlock;
      friend class SubqueryBlock;

////////////////////////////////////////////////////////////////////////////////
/// @brief constructor
////////////////////////////////////////////////////////////////////////////////

      public:

        SubqueryNode (ExecutionPlan*,
                      triagens::basics::Json const& base);

        SubqueryNode (ExecutionPlan* plan,
                      size_t id,
                      ExecutionNode* subquery, 
                      Variable const* outVariable)
          : ExecutionNode(plan, id), 
            _subquery(subquery), 
            _outVariable(outVariable) {

          TRI_ASSERT(_subquery != nullptr);
          TRI_ASSERT(_outVariable != nullptr);
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the type of the node
////////////////////////////////////////////////////////////////////////////////

        NodeType getType () const override {
          return SUBQUERY;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief export to JSON
////////////////////////////////////////////////////////////////////////////////

        virtual void toJsonHelper (triagens::basics::Json&,
                                   TRI_memory_zone_t*,
                                   bool) const override;

////////////////////////////////////////////////////////////////////////////////
/// @brief clone ExecutionNode recursively
////////////////////////////////////////////////////////////////////////////////

        virtual ExecutionNode* clone (ExecutionPlan* plan,
                                      bool withDependencies,
                                      bool withProperties) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief getter for subquery
////////////////////////////////////////////////////////////////////////////////

        ExecutionNode* getSubquery () const {
          return _subquery;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief setter for subquery
////////////////////////////////////////////////////////////////////////////////

        void setSubquery (ExecutionNode* subquery) {
          TRI_ASSERT(subquery != nullptr);
          TRI_ASSERT(_subquery == nullptr); // do not allow overwriting an existing subquery
          _subquery = subquery;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief the cost of a subquery node is the cost of its unique dependency
/// times a small constant
////////////////////////////////////////////////////////////////////////////////
        
        double estimateCost () override {
          return 1.005 * _dependencies.at(0)->getCost();
          //FIXME improve this estimate . . .
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesUsedHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesUsedHere () const;

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesSetHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesSetHere () const {
          std::vector<Variable const*> v;
          v.push_back(_outVariable);
          return v;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief can the node throw? Note that this means that an exception can
/// *originate* from this node. That is, this method does not need to
/// return true just because a dependent node can throw an exception.
////////////////////////////////////////////////////////////////////////////////

        bool canThrow ();

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief we need to have an expression and where to write the result
////////////////////////////////////////////////////////////////////////////////

        ExecutionNode* _subquery;

////////////////////////////////////////////////////////////////////////////////
/// @brief variable to write to
////////////////////////////////////////////////////////////////////////////////

        Variable const* _outVariable;

    };

// -----------------------------------------------------------------------------
// --SECTION--                                                  class FilterNode
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief class FilterNode
////////////////////////////////////////////////////////////////////////////////

    class FilterNode : public ExecutionNode {
      
      friend class ExecutionBlock;
      friend class FilterBlock;
      friend class RedundantCalculationsReplacer;

////////////////////////////////////////////////////////////////////////////////
/// @brief constructors for various arguments, always with offset and limit
////////////////////////////////////////////////////////////////////////////////

      public:

        FilterNode (ExecutionPlan* plan,
                    size_t id,
                    Variable const* inVariable)
          : ExecutionNode(plan, id), 
            _inVariable(inVariable) {

          TRI_ASSERT(_inVariable != nullptr);
        }
        
        FilterNode (ExecutionPlan*, triagens::basics::Json const& base);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the type of the node
////////////////////////////////////////////////////////////////////////////////

        NodeType getType () const override {
          return FILTER;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief export to JSON
////////////////////////////////////////////////////////////////////////////////

        virtual void toJsonHelper (triagens::basics::Json&,
                                   TRI_memory_zone_t*,
                                   bool) const override;

////////////////////////////////////////////////////////////////////////////////
/// @brief clone ExecutionNode recursively
////////////////////////////////////////////////////////////////////////////////

        virtual ExecutionNode* clone (ExecutionPlan* plan,
                                      bool withDependencies,
                                      bool withProperties) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief the cost of a filter node is . . . FIXME
////////////////////////////////////////////////////////////////////////////////
        
        double estimateCost () override {
          return _dependencies.at(0)->getCost() * 0.105;
          //FIXME! 0.005 is the cost of doing the filter node under the
          //assumption that it returns 10% of the results of its dependency
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesUsedHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesUsedHere () const {
          std::vector<Variable const*> v;
          v.push_back(_inVariable);
          return v;
        }

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief input variable to read from
////////////////////////////////////////////////////////////////////////////////

        Variable const* _inVariable;

    };

// -----------------------------------------------------------------------------
// --SECTION--                                            struct SortInformation
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief this is an auxilliary struct for processed sort criteria information
////////////////////////////////////////////////////////////////////////////////
    
    struct SortInformation {

      enum Match {
        unequal,                // criteria are unequal
        otherLessAccurate,      // leftmost sort criteria are equal, but other sort criteria are less accurate than ourselves
        ourselvesLessAccurate,  // leftmost sort criteria are equal, but our own sort criteria is less accurate than the other
        allEqual                // all criteria are equal
      };

      std::vector<std::tuple<ExecutionNode const*, std::string, bool>> criteria;
      bool isValid   = true;
      bool isComplex = false;
      bool canThrow  = false;
          
      Match isCoveredBy (SortInformation const& other) {
        if (! isValid || ! other.isValid) {
          return unequal;
        }

        if (isComplex || other.isComplex) {
          return unequal;
        }

        size_t const n = criteria.size();
        for (size_t i = 0; i < n; ++i) {
          if (other.criteria.size() <= i) {
            return otherLessAccurate;
          }

          auto ours   = criteria[i];
          auto theirs = other.criteria[i];

          if (std::get<2>(ours) != std::get<2>(theirs)) {
            // sort order is different
            return unequal;
          }

          if (std::get<1>(ours) != std::get<1>(theirs)) {
            // sort criterion is different
            return unequal;
          }
        }

        if (other.criteria.size() > n) {
          return ourselvesLessAccurate;
        }
          
        return allEqual;
      }
    };

// -----------------------------------------------------------------------------
// --SECTION--                                                    class SortNode
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief class SortNode
////////////////////////////////////////////////////////////////////////////////

    class SortNode : public ExecutionNode {
      
      friend class ExecutionBlock;
      friend class SortBlock;
      friend class RedundantCalculationsReplacer;

////////////////////////////////////////////////////////////////////////////////
/// @brief constructor
////////////////////////////////////////////////////////////////////////////////

      public:

        SortNode (ExecutionPlan* plan,
                  size_t id,
                  SortElementVector const& elements,
                  bool stable) 
          : ExecutionNode(plan, id),
            _elements(elements),
            _stable(stable) {
        }
        
        SortNode (ExecutionPlan* plan,
                  triagens::basics::Json const& base,
                  SortElementVector const& elements,
                  bool stable);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the type of the node
////////////////////////////////////////////////////////////////////////////////

        NodeType getType () const override {
          return SORT;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not the sort is stable
////////////////////////////////////////////////////////////////////////////////

        inline bool isStable () const {
          return _stable;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief export to JSON
////////////////////////////////////////////////////////////////////////////////

        virtual void toJsonHelper (triagens::basics::Json&,
                                   TRI_memory_zone_t*,
                                   bool) const override;

////////////////////////////////////////////////////////////////////////////////
/// @brief clone ExecutionNode recursively
////////////////////////////////////////////////////////////////////////////////

        virtual ExecutionNode* clone (ExecutionPlan* plan,
                                      bool withDependencies,
                                      bool withProperties) const {
          auto c = new SortNode(plan, _id, _elements, _stable);

          CloneHelper (c, plan, withDependencies, withProperties);

          return static_cast<ExecutionNode*>(c);
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief the cost of a sort node is . . . FIXME
////////////////////////////////////////////////////////////////////////////////
        
        double estimateCost () override {
          double depCost = _dependencies.at(0)->getCost();
          if (depCost <= 2.0) {
            return depCost;
          }
          else {
            return log(depCost) * depCost;
          }
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesUsedHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesUsedHere () const {
          std::vector<Variable const*> v;
          for (auto p : _elements) {
            v.push_back(p.first);
          }
          return v;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief get Variables Used Here including ASC/DESC
////////////////////////////////////////////////////////////////////////////////

        SortElementVector const & getElements () const {
          return _elements;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief returns all sort information 
////////////////////////////////////////////////////////////////////////////////

        SortInformation getSortInformation (ExecutionPlan*,
                                            triagens::basics::StringBuffer*) const;

        std::vector<std::pair<ExecutionNode*, bool>> getCalcNodePairs ();

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief pairs, consisting of variable and sort direction
/// (true = ascending | false = descending)
////////////////////////////////////////////////////////////////////////////////

        SortElementVector _elements;

////////////////////////////////////////////////////////////////////////////////
/// whether or not the sort is stable
////////////////////////////////////////////////////////////////////////////////

        bool _stable;
    };


// -----------------------------------------------------------------------------
// --SECTION--                                               class AggregateNode
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief class AggregateNode
////////////////////////////////////////////////////////////////////////////////

    class AggregateNode : public ExecutionNode {
      
      friend class ExecutionNode;
      friend class ExecutionBlock;
      friend class AggregateBlock;
      friend class RedundantCalculationsReplacer;

////////////////////////////////////////////////////////////////////////////////
/// @brief constructor
////////////////////////////////////////////////////////////////////////////////

      public:

        AggregateNode (ExecutionPlan* plan,
                       size_t id,
                       std::vector<std::pair<Variable const*, Variable const*>> aggregateVariables,
                       Variable const* outVariable,
                       std::unordered_map<VariableId, std::string const> const& variableMap)
          : ExecutionNode(plan, id), 
            _aggregateVariables(aggregateVariables), 
            _outVariable(outVariable),
            _variableMap(variableMap) {
          // outVariable can be a nullptr
        }
        
        AggregateNode (ExecutionPlan*,
                       triagens::basics::Json const& base,
                       Variable const* outVariable,
                       std::unordered_map<VariableId, std::string const> const& variableMap,
                       std::vector<std::pair<Variable const*, Variable const*>> aggregateVariables);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the type of the node
////////////////////////////////////////////////////////////////////////////////

        NodeType getType () const override {
          return AGGREGATE;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief export to JSON
////////////////////////////////////////////////////////////////////////////////

        virtual void toJsonHelper (triagens::basics::Json&,
                                   TRI_memory_zone_t*,
                                   bool) const override;

////////////////////////////////////////////////////////////////////////////////
/// @brief clone ExecutionNode recursively
////////////////////////////////////////////////////////////////////////////////

        virtual ExecutionNode* clone (ExecutionPlan* plan,
                                      bool withDependencies,
                                      bool withProperties) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief the cost of an aggregate node is . . . FIXME
////////////////////////////////////////////////////////////////////////////////
        
        double estimateCost () override {
          return 2 * _dependencies.at(0)->getCost();
          //FIXME improve this estimate . . .
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not the node has an outVariable (i.e. INTO ...)
////////////////////////////////////////////////////////////////////////////////

        inline bool hasOutVariable () const {
          return _outVariable != nullptr;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesUsedHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesUsedHere () const;

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesSetHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesSetHere () const {
          std::vector<Variable const*> v;
          for (auto p : _aggregateVariables) {
            v.push_back(p.first);
          }
          if (_outVariable != nullptr) {
            v.push_back(_outVariable);
          }
          return v;
        }

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief input/output variables for the aggregation (out, in)
////////////////////////////////////////////////////////////////////////////////

        std::vector<std::pair<Variable const*, Variable const*>> _aggregateVariables;

////////////////////////////////////////////////////////////////////////////////
/// @brief output variable to write to (might be null)
////////////////////////////////////////////////////////////////////////////////

        Variable const* _outVariable;

////////////////////////////////////////////////////////////////////////////////
/// @brief map of all variable ids and names (needed to construct group data)
////////////////////////////////////////////////////////////////////////////////
                       
        std::unordered_map<VariableId, std::string const> const _variableMap;

    };


// -----------------------------------------------------------------------------
// --SECTION--                                                  class ReturnNode
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief class ReturnNode
////////////////////////////////////////////////////////////////////////////////

    class ReturnNode : public ExecutionNode {
      
      friend class ExecutionBlock;
      friend class ReturnBlock;
      friend class RedundantCalculationsReplacer;
      
////////////////////////////////////////////////////////////////////////////////
/// @brief constructors for various arguments, always with offset and limit
////////////////////////////////////////////////////////////////////////////////

      public:

        ReturnNode (ExecutionPlan* plan,
                    size_t id,
                    Variable const* inVariable)
          : ExecutionNode(plan, id), 
            _inVariable(inVariable) {

          TRI_ASSERT(_inVariable != nullptr);
        }

        ReturnNode (ExecutionPlan*, triagens::basics::Json const& base);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the type of the node
////////////////////////////////////////////////////////////////////////////////

        NodeType getType () const override {
          return RETURN;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief export to JSON
////////////////////////////////////////////////////////////////////////////////

        virtual void toJsonHelper (triagens::basics::Json&,
                                   TRI_memory_zone_t*,
                                   bool) const override;

////////////////////////////////////////////////////////////////////////////////
/// @brief clone ExecutionNode recursively
////////////////////////////////////////////////////////////////////////////////

        virtual ExecutionNode* clone (ExecutionPlan* plan,
                                      bool withDependencies,
                                      bool withProperties) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief the cost of a return node is the cost of its only dependency . . .
////////////////////////////////////////////////////////////////////////////////
        
        double estimateCost () override {
          return _dependencies.at(0)->getCost();
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesUsedHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesUsedHere () const {
          std::vector<Variable const*> v;
          v.push_back(_inVariable);
          return v;
        }

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief we need to know the offset and limit
////////////////////////////////////////////////////////////////////////////////

        Variable const* _inVariable;

    };

// -----------------------------------------------------------------------------
// --SECTION--                                            class ModificationNode
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief abstract base class for modification operations
////////////////////////////////////////////////////////////////////////////////

    class ModificationNode : public ExecutionNode {
      
      friend class ExecutionBlock;
      friend class ModificationBlock;
      
////////////////////////////////////////////////////////////////////////////////
/// @brief constructor with a vocbase and a collection and options
////////////////////////////////////////////////////////////////////////////////

      protected:

        ModificationNode (ExecutionPlan* plan,
                          size_t id,
                          TRI_vocbase_t* vocbase, 
                          Collection* collection,
                          ModificationOptions const& options)
          : ExecutionNode(plan, id), 
            _vocbase(vocbase), 
            _collection(collection),
            _options(options) {

          TRI_ASSERT(_vocbase != nullptr);
          TRI_ASSERT(_collection != nullptr);
        }

        ModificationNode (ExecutionPlan*,
                          triagens::basics::Json const& json);


////////////////////////////////////////////////////////////////////////////////
/// @brief export to JSON
////////////////////////////////////////////////////////////////////////////////

        virtual void toJsonHelper (triagens::basics::Json& json,
                                   TRI_memory_zone_t* zone,
                                   bool) const override;

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief return the database
////////////////////////////////////////////////////////////////////////////////

        TRI_vocbase_t* vocbase () const {
          return _vocbase;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the collection
////////////////////////////////////////////////////////////////////////////////

        Collection const* collection () const {
          return _collection;
        }

// -----------------------------------------------------------------------------
// --SECTION--                                               protected variables
// -----------------------------------------------------------------------------

      protected:

////////////////////////////////////////////////////////////////////////////////
/// @brief _vocbase, the database
////////////////////////////////////////////////////////////////////////////////

        TRI_vocbase_t* _vocbase;

////////////////////////////////////////////////////////////////////////////////
/// @brief collection
////////////////////////////////////////////////////////////////////////////////

        Collection* _collection;

////////////////////////////////////////////////////////////////////////////////
/// @brief modification operation options
////////////////////////////////////////////////////////////////////////////////

        ModificationOptions _options;

    };


// -----------------------------------------------------------------------------
// --SECTION--                                                  class RemoveNode
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief class RemoveNode
////////////////////////////////////////////////////////////////////////////////

    class RemoveNode : public ModificationNode {
      
      friend class ExecutionNode;
      friend class ExecutionBlock;
      friend class RemoveBlock;
      
////////////////////////////////////////////////////////////////////////////////
/// @brief constructor 
////////////////////////////////////////////////////////////////////////////////

      public:

        RemoveNode (ExecutionPlan* plan,
                    size_t id,
                    TRI_vocbase_t* vocbase, 
                    Collection* collection,
                    ModificationOptions const& options,
                    Variable const* inVariable,
                    Variable const* outVariable)
          : ModificationNode(plan, id, vocbase, collection, options),
            _inVariable(inVariable),
            _outVariable(outVariable) {

          TRI_ASSERT(_inVariable != nullptr);
          // _outVariable might be a nullptr
        }
        
        RemoveNode (ExecutionPlan*, triagens::basics::Json const& base);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the type of the node
////////////////////////////////////////////////////////////////////////////////

        NodeType getType () const override {
          return REMOVE;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief export to JSON
////////////////////////////////////////////////////////////////////////////////

        virtual void toJsonHelper (triagens::basics::Json&,
                                   TRI_memory_zone_t*,
                                   bool) const override;

////////////////////////////////////////////////////////////////////////////////
/// @brief clone ExecutionNode recursively
////////////////////////////////////////////////////////////////////////////////

        virtual ExecutionNode* clone (ExecutionPlan* plan,
                                      bool withDependencies,
                                      bool withProperties) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief the cost of a remove node is a multiple of the cost of its unique 
/// dependency
////////////////////////////////////////////////////////////////////////////////
        
        double estimateCost () override {
          return _dependencies.at(0)->getCost();
          // TODO: improve this estimate!
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesUsedHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesUsedHere () const {
          std::vector<Variable const*> v;
          v.push_back(_inVariable);
          return v;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesSetHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesSetHere () const {
          std::vector<Variable const*> v;
          if (_outVariable != nullptr) {
            v.push_back(_outVariable);
          }
          return v;
        }

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief input variable
////////////////////////////////////////////////////////////////////////////////

        Variable const* _inVariable;

////////////////////////////////////////////////////////////////////////////////
/// @brief output variable (might be a nullptr)
////////////////////////////////////////////////////////////////////////////////

        Variable const* _outVariable;

    };

// -----------------------------------------------------------------------------
// --SECTION--                                                  class InsertNode
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief class InsertNode
////////////////////////////////////////////////////////////////////////////////

    class InsertNode : public ModificationNode {
      
      friend class ExecutionNode;
      friend class ExecutionBlock;
      friend class InsertBlock;
      
////////////////////////////////////////////////////////////////////////////////
/// @brief constructor 
////////////////////////////////////////////////////////////////////////////////

      public:

        InsertNode (ExecutionPlan* plan,
                    size_t id,
                    TRI_vocbase_t* vocbase, 
                    Collection* collection,
                    ModificationOptions const& options,
                    Variable const* inVariable,
                    Variable const* outVariable)
          : ModificationNode(plan, id, vocbase, collection, options),
            _inVariable(inVariable),
            _outVariable(outVariable) {

          TRI_ASSERT(_inVariable != nullptr);
          // _outVariable might be a nullptr
        }
        
        InsertNode (ExecutionPlan*, triagens::basics::Json const& base);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the type of the node
////////////////////////////////////////////////////////////////////////////////

        NodeType getType () const override {
          return INSERT;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief export to JSON
////////////////////////////////////////////////////////////////////////////////

        virtual void toJsonHelper (triagens::basics::Json&,
                                   TRI_memory_zone_t*,
                                   bool) const override;

////////////////////////////////////////////////////////////////////////////////
/// @brief clone ExecutionNode recursively
////////////////////////////////////////////////////////////////////////////////

        virtual ExecutionNode* clone (ExecutionPlan* plan,
                                      bool withDependencies,
                                      bool withProperties) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief the cost of a remove node is a multiple of the cost of its unique 
/// dependency
////////////////////////////////////////////////////////////////////////////////
        
        double estimateCost () override {
          return 1000 * _dependencies.at(0)->getCost(); //FIXME change this!
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesUsedHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesUsedHere () const {
          std::vector<Variable const*> v;
          v.push_back(_inVariable);
          return v;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesSetHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesSetHere () const {
          std::vector<Variable const*> v;
          if (_outVariable != nullptr) {
            v.push_back(_outVariable);
          }
          return v;
        }

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief input variable
////////////////////////////////////////////////////////////////////////////////

        Variable const* _inVariable;

////////////////////////////////////////////////////////////////////////////////
/// @brief output variable
////////////////////////////////////////////////////////////////////////////////

        Variable const* _outVariable;

    };

// -----------------------------------------------------------------------------
// --SECTION--                                                  class UpdateNode
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief class UpdateNode
////////////////////////////////////////////////////////////////////////////////

    class UpdateNode : public ModificationNode {
      
      friend class ExecutionNode;
      friend class ExecutionBlock;
      friend class UpdateBlock;
      
////////////////////////////////////////////////////////////////////////////////
/// @brief constructor with a vocbase and a collection name
////////////////////////////////////////////////////////////////////////////////

      public:

        UpdateNode (ExecutionPlan* plan,
                    size_t id, 
                    TRI_vocbase_t* vocbase, 
                    Collection* collection,
                    ModificationOptions const& options,
                    Variable const* inDocVariable,
                    Variable const* inKeyVariable,
                    Variable const* outVariable)
          : ModificationNode(plan, id, vocbase, collection, options),
            _inDocVariable(inDocVariable),
            _inKeyVariable(inKeyVariable),
            _outVariable(outVariable) {

          TRI_ASSERT(_inDocVariable != nullptr);
          // _inKeyVariable might be a nullptr
          // _outVariable might be a nullptr
        }
        
        UpdateNode (ExecutionPlan*, triagens::basics::Json const& base);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the type of the node
////////////////////////////////////////////////////////////////////////////////

        NodeType getType () const override {
          return UPDATE;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief export to JSON
////////////////////////////////////////////////////////////////////////////////

        virtual void toJsonHelper (triagens::basics::Json&,
                                   TRI_memory_zone_t*,
                                   bool) const override;

////////////////////////////////////////////////////////////////////////////////
/// @brief clone ExecutionNode recursively
////////////////////////////////////////////////////////////////////////////////

        virtual ExecutionNode* clone (ExecutionPlan* plan,
                                      bool withDependencies,
                                      bool withProperties) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief the cost of a remove node is a multiple of the cost of its unique 
/// dependency
////////////////////////////////////////////////////////////////////////////////
        
        double estimateCost () override {
          return 1000 * _dependencies.at(0)->getCost(); //FIXME change this!
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesUsedHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesUsedHere () const {
          std::vector<Variable const*> v;
          v.push_back(_inDocVariable);

          if (_inKeyVariable != nullptr) {
            v.push_back(_inKeyVariable);
          }
          return v;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesSetHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesSetHere () const {
          std::vector<Variable const*> v;
          if (_outVariable != nullptr) {
            v.push_back(_outVariable);
          }
          return v;
        }

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief input variable for documents
////////////////////////////////////////////////////////////////////////////////

        Variable const* _inDocVariable;

////////////////////////////////////////////////////////////////////////////////
/// @brief input variable for keys
////////////////////////////////////////////////////////////////////////////////

        Variable const* _inKeyVariable;

////////////////////////////////////////////////////////////////////////////////
/// @brief output variable
////////////////////////////////////////////////////////////////////////////////

        Variable const* _outVariable;

    };

// -----------------------------------------------------------------------------
// --SECTION--                                                 class ReplaceNode
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief class ReplaceNode
////////////////////////////////////////////////////////////////////////////////

    class ReplaceNode : public ModificationNode {
      
      friend class ExecutionNode;
      friend class ExecutionBlock;
      friend class ReplaceBlock;
      
////////////////////////////////////////////////////////////////////////////////
/// @brief constructor with a vocbase and a collection name
////////////////////////////////////////////////////////////////////////////////

      public:

        ReplaceNode (ExecutionPlan* plan,
                     size_t id,
                     TRI_vocbase_t* vocbase, 
                     Collection* collection,
                     ModificationOptions const& options,
                     Variable const* inDocVariable,
                     Variable const* inKeyVariable,
                     Variable const* outVariable)
          : ModificationNode(plan, id, vocbase, collection, options),
            _inDocVariable(inDocVariable),
            _inKeyVariable(inKeyVariable),
            _outVariable(outVariable) {

          TRI_ASSERT(_inDocVariable != nullptr);
          // _inKeyVariable might be a nullptr
          // _outVariable might be a nullptr
        }

        ReplaceNode (ExecutionPlan*, triagens::basics::Json const& base);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the type of the node
////////////////////////////////////////////////////////////////////////////////

        NodeType getType () const override {
          return REPLACE;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief export to JSON
////////////////////////////////////////////////////////////////////////////////

        virtual void toJsonHelper (triagens::basics::Json&,
                                   TRI_memory_zone_t*,
                                   bool) const override;

////////////////////////////////////////////////////////////////////////////////
/// @brief clone ExecutionNode recursively
////////////////////////////////////////////////////////////////////////////////

        virtual ExecutionNode* clone (ExecutionPlan* plan,
                                      bool withDependencies,
                                      bool withProperties) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief the cost of a remove node is a multiple of the cost of its unique 
/// dependency
////////////////////////////////////////////////////////////////////////////////
        
        double estimateCost () override {
          return 1000 * _dependencies.at(0)->getCost(); //FIXME change this!
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesUsedHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesUsedHere () const {
          std::vector<Variable const*> v;
          v.push_back(_inDocVariable);

          if (_inKeyVariable != nullptr) {
            v.push_back(_inKeyVariable);
          }
          return v;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesSetHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesSetHere () const {
          std::vector<Variable const*> v;
          if (_outVariable != nullptr) {
            v.push_back(_outVariable);
          }
          return v;
        }

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief input variable for documents
////////////////////////////////////////////////////////////////////////////////

        Variable const* _inDocVariable;

////////////////////////////////////////////////////////////////////////////////
/// @brief input variable for keys
////////////////////////////////////////////////////////////////////////////////

        Variable const* _inKeyVariable;

////////////////////////////////////////////////////////////////////////////////
/// @brief output variable
////////////////////////////////////////////////////////////////////////////////

        Variable const* _outVariable;

    };

// -----------------------------------------------------------------------------
// --SECTION--                                               class NoResultsNode
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief class NoResultsNode
////////////////////////////////////////////////////////////////////////////////

    class NoResultsNode : public ExecutionNode {
      
      friend class ExecutionBlock;
      friend class NoResultsBlock;
      
////////////////////////////////////////////////////////////////////////////////
/// @brief constructor with an id
////////////////////////////////////////////////////////////////////////////////

      public:
  
        NoResultsNode (ExecutionPlan* plan, size_t id) 
          : ExecutionNode(plan, id) {
        }

        NoResultsNode (ExecutionPlan* plan, triagens::basics::Json const& base)
          : ExecutionNode(plan, base) {
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the type of the node
////////////////////////////////////////////////////////////////////////////////

        NodeType getType () const override {
          return NORESULTS;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief export to JSON
////////////////////////////////////////////////////////////////////////////////

        virtual void toJsonHelper (triagens::basics::Json&,
                                   TRI_memory_zone_t*,
                                   bool) const override;

////////////////////////////////////////////////////////////////////////////////
/// @brief clone ExecutionNode recursively
////////////////////////////////////////////////////////////////////////////////

        virtual ExecutionNode* clone (ExecutionPlan* plan,
                                      bool withDependencies,
                                      bool withProperties) const {
          auto c = new NoResultsNode(plan, _id);

          CloneHelper (c, plan, withDependencies, withProperties);

          return static_cast<ExecutionNode*>(c);
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief the cost of a NoResults is 0
////////////////////////////////////////////////////////////////////////////////
        
        double estimateCost () override {
          return 0;
        }

    };

// -----------------------------------------------------------------------------
// --SECTION--                                                  class RemoteNode
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief class RemoteNode
////////////////////////////////////////////////////////////////////////////////

    class RemoteNode : public ExecutionNode {
      
      friend class ExecutionBlock;
      friend class RemoteBlock;
      
////////////////////////////////////////////////////////////////////////////////
/// @brief constructor with an id
////////////////////////////////////////////////////////////////////////////////

      public:
 
        RemoteNode (ExecutionPlan* plan, 
                    size_t id,
                    TRI_vocbase_t* vocbase,
                    Collection const* collection,
                    std::string const& server,
                    std::string const& ownName,
                    std::string const& queryId) 
          : ExecutionNode(plan, id),
            _vocbase(vocbase),
            _collection(collection),
            _server(server),
            _ownName(ownName),
            _queryId(queryId) {
          // note: server, ownName and queryId may be empty and filled later
        }

        RemoteNode (ExecutionPlan*, triagens::basics::Json const& base);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the type of the node
////////////////////////////////////////////////////////////////////////////////

        NodeType getType () const override {
          return REMOTE;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief export to JSON
////////////////////////////////////////////////////////////////////////////////

        virtual void toJsonHelper (triagens::basics::Json&,
                                   TRI_memory_zone_t*,
                                   bool) const override;

////////////////////////////////////////////////////////////////////////////////
/// @brief clone ExecutionNode recursively
////////////////////////////////////////////////////////////////////////////////

        virtual ExecutionNode* clone (ExecutionPlan* plan,
                                      bool withDependencies,
                                      bool withProperties) const {
          auto c = new RemoteNode(plan, _id, _vocbase, _collection, _server, _ownName, _queryId);

          CloneHelper (c, plan, withDependencies, withProperties);

          return static_cast<ExecutionNode*>(c);
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief the cost of a remote node is that of its dependency
////////////////////////////////////////////////////////////////////////////////
        
        double estimateCost () {
          if (_dependencies.size() == 1) {
            // the 1.5 is an arbitrary factor to account for some overhead of the
            // remote processing, HTTP communication etc.
            return 1.5 * _dependencies[0]->estimateCost();
          }
          return 1.5;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the database
////////////////////////////////////////////////////////////////////////////////

        TRI_vocbase_t* vocbase () const {
          return _vocbase;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the collection
////////////////////////////////////////////////////////////////////////////////

        Collection const* collection () const {
          return _collection;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the server name
////////////////////////////////////////////////////////////////////////////////

        std::string server () const {
          return _server;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief set the server name
////////////////////////////////////////////////////////////////////////////////

        void server (std::string const& server) {
          _server = server;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return our own name
////////////////////////////////////////////////////////////////////////////////
        
        std::string ownName () const {
          return _ownName;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief set our own name
////////////////////////////////////////////////////////////////////////////////
        
        void ownName (std::string const& ownName) {
          _ownName = ownName;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the query id
////////////////////////////////////////////////////////////////////////////////

        std::string queryId () const {
          return _queryId;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief set the query id
////////////////////////////////////////////////////////////////////////////////

        void queryId (std::string const& queryId) {
          _queryId = queryId;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief set the query id
////////////////////////////////////////////////////////////////////////////////

        void queryId (QueryId queryId) {
          _queryId = triagens::basics::StringUtils::itoa(queryId);
        }

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief the underlying database
////////////////////////////////////////////////////////////////////////////////
                                 
        TRI_vocbase_t* _vocbase;

////////////////////////////////////////////////////////////////////////////////
/// @brief the underlying collection
////////////////////////////////////////////////////////////////////////////////

        Collection const* _collection;

////////////////////////////////////////////////////////////////////////////////
/// @brief our server, can be like "shard:S1000" or like "server:Claus"
////////////////////////////////////////////////////////////////////////////////

        std::string _server;

////////////////////////////////////////////////////////////////////////////////
/// @brief our own identity, in case of the coordinator this is empty,
/// in case of the DBservers, this is the shard ID as a string
////////////////////////////////////////////////////////////////////////////////

        std::string _ownName;

////////////////////////////////////////////////////////////////////////////////
/// @brief the ID of the query on the server as a string
////////////////////////////////////////////////////////////////////////////////

        std::string _queryId;

    };

// -----------------------------------------------------------------------------
// --SECTION--                                                 class ScatterNode
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief class ScatterNode
////////////////////////////////////////////////////////////////////////////////

    class ScatterNode : public ExecutionNode {
      
      friend class ExecutionBlock;
      friend class ScatterBlock;
      
////////////////////////////////////////////////////////////////////////////////
/// @brief constructor with an id
////////////////////////////////////////////////////////////////////////////////

      public:
 
        ScatterNode (ExecutionPlan* plan, 
                     size_t id,
                     TRI_vocbase_t* vocbase,
                     Collection const* collection) 
          : ExecutionNode(plan, id),
            _vocbase(vocbase),
            _collection(collection) {
        }

        ScatterNode (ExecutionPlan*, 
                     triagens::basics::Json const& base);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the type of the node
////////////////////////////////////////////////////////////////////////////////

        NodeType getType () const override {
          return SCATTER;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief export to JSON
////////////////////////////////////////////////////////////////////////////////

        virtual void toJsonHelper (triagens::basics::Json&,
                                   TRI_memory_zone_t*,
                                   bool) const override;

////////////////////////////////////////////////////////////////////////////////
/// @brief clone ExecutionNode recursively
////////////////////////////////////////////////////////////////////////////////

        virtual ExecutionNode* clone (ExecutionPlan* plan,
                                      bool withDependencies,
                                      bool withProperties) const {
          auto c = new ScatterNode(plan, _id, _vocbase, _collection);

          CloneHelper (c, plan, withDependencies, withProperties);

          return static_cast<ExecutionNode*>(c);
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief the cost of a scatter node is 1
////////////////////////////////////////////////////////////////////////////////
        
        double estimateCost () {
          return 1;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the database
////////////////////////////////////////////////////////////////////////////////

        TRI_vocbase_t* vocbase () const {
          return _vocbase;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the collection
////////////////////////////////////////////////////////////////////////////////

        Collection const* collection () const {
          return _collection;
        }

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief the underlying database
////////////////////////////////////////////////////////////////////////////////
                                 
        TRI_vocbase_t* _vocbase;

////////////////////////////////////////////////////////////////////////////////
/// @brief the underlying collection
////////////////////////////////////////////////////////////////////////////////

        Collection const* _collection;

    };

// -----------------------------------------------------------------------------
// --SECTION--                                              class DistributeNode
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief class DistributeNode
////////////////////////////////////////////////////////////////////////////////

    class DistributeNode : public ExecutionNode {
      
      friend class ExecutionBlock;
      friend class DistributeBlock;
      
////////////////////////////////////////////////////////////////////////////////
/// @brief constructor with an id
////////////////////////////////////////////////////////////////////////////////

      public:
 
        DistributeNode (ExecutionPlan* plan, 
                        size_t id,
                        TRI_vocbase_t* vocbase,
                        Collection const* collection)
          : ExecutionNode(plan, id),
            _vocbase(vocbase),
            _collection(collection) {
        }

        DistributeNode (ExecutionPlan*, 
                        triagens::basics::Json const& base);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the type of the node
////////////////////////////////////////////////////////////////////////////////

        NodeType getType () const override {
          return DISTRIBUTE;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief export to JSON
////////////////////////////////////////////////////////////////////////////////

        virtual void toJsonHelper (triagens::basics::Json&,
                                   TRI_memory_zone_t*,
                                   bool) const override;

////////////////////////////////////////////////////////////////////////////////
/// @brief clone ExecutionNode recursively
////////////////////////////////////////////////////////////////////////////////

        virtual ExecutionNode* clone (ExecutionPlan* plan,
                                      bool withDependencies,
                                      bool withProperties) const {
          auto c = new DistributeNode(plan, _id, _vocbase, _collection);

          CloneHelper (c, plan, withDependencies, withProperties);

          return static_cast<ExecutionNode*>(c);
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief the cost of a Distribute node is 1
////////////////////////////////////////////////////////////////////////////////
        
        double estimateCost () {
          return 1;
        }
      
////////////////////////////////////////////////////////////////////////////////
/// @brief return the database
////////////////////////////////////////////////////////////////////////////////

        TRI_vocbase_t* vocbase () const {
          return _vocbase;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the collection
////////////////////////////////////////////////////////////////////////////////

        Collection const* collection () const {
          return _collection;
        }

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief the underlying database
////////////////////////////////////////////////////////////////////////////////
                                 
        TRI_vocbase_t* _vocbase;

////////////////////////////////////////////////////////////////////////////////
/// @brief the underlying collection
////////////////////////////////////////////////////////////////////////////////

        Collection const* _collection;

    };

// -----------------------------------------------------------------------------
// --SECTION--                                                  class GatherNode
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief class GatherNode
////////////////////////////////////////////////////////////////////////////////

    class GatherNode : public ExecutionNode {
      
      friend class ExecutionBlock;
      friend class GatherBlock;
      
////////////////////////////////////////////////////////////////////////////////
/// @brief constructor with an id
////////////////////////////////////////////////////////////////////////////////

      public:
 
        GatherNode (ExecutionPlan* plan, 
                    size_t id,
                    TRI_vocbase_t* vocbase,
                    Collection const* collection)
          : ExecutionNode(plan, id),
            _vocbase(vocbase),
            _collection(collection) {
        }

        GatherNode (ExecutionPlan*,
                    triagens::basics::Json const& base,
                    SortElementVector const& elements);

////////////////////////////////////////////////////////////////////////////////
/// @brief return the type of the node
////////////////////////////////////////////////////////////////////////////////

        NodeType getType () const override {
          return GATHER;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief export to JSON
////////////////////////////////////////////////////////////////////////////////

        virtual void toJsonHelper (triagens::basics::Json&,
                                   TRI_memory_zone_t*,
                                   bool) const override;

////////////////////////////////////////////////////////////////////////////////
/// @brief clone ExecutionNode recursively
////////////////////////////////////////////////////////////////////////////////

        virtual ExecutionNode* clone (ExecutionPlan* plan,
                                      bool withDependencies,
                                      bool withProperties) const {
          auto c = new GatherNode(plan, _id, _vocbase, _collection);

          CloneHelper (c, plan, withDependencies, withProperties);

          return static_cast<ExecutionNode*>(c);
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief the cost of a gather node is 1
////////////////////////////////////////////////////////////////////////////////
        
        double estimateCost () {
          return 1;
        }
      
////////////////////////////////////////////////////////////////////////////////
/// @brief getVariablesUsedHere
////////////////////////////////////////////////////////////////////////////////

        virtual std::vector<Variable const*> getVariablesUsedHere () const {
          std::vector<Variable const*> v;
          for (auto p : _elements) {
            v.push_back(p.first);
          }
          return v;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief get Variables Used Here including ASC/DESC
////////////////////////////////////////////////////////////////////////////////

        SortElementVector const & getElements () const {
          return _elements;
        }

        void setElements (SortElementVector const & src) {
          _elements = src;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the database
////////////////////////////////////////////////////////////////////////////////

        TRI_vocbase_t* vocbase () const {
          return _vocbase;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the collection
////////////////////////////////////////////////////////////////////////////////

        Collection const* collection () const {
          return _collection;
        }

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief pairs, consisting of variable and sort direction
/// (true = ascending | false = descending)
////////////////////////////////////////////////////////////////////////////////

        SortElementVector _elements;

////////////////////////////////////////////////////////////////////////////////
/// @brief the underlying database
////////////////////////////////////////////////////////////////////////////////
                                 
        TRI_vocbase_t* _vocbase;

////////////////////////////////////////////////////////////////////////////////
/// @brief the underlying collection
////////////////////////////////////////////////////////////////////////////////

        Collection const* _collection;

    };

  }   // namespace triagens::aql
}  // namespace triagens

#endif

// Local Variables:
// mode: outline-minor
// outline-regexp: "^\\(/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|// --SECTION--\\|/// @\\}\\)"
// End:


