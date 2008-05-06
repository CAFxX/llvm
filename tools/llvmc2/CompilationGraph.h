//===--- CompilationGraph.h - The LLVM Compiler Driver ----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open
// Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  Compilation graph - definition.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVMC2_COMPILATION_GRAPH_H
#define LLVM_TOOLS_LLVMC2_COMPILATION_GRAPH_H

#include "AutoGenerated.h"
#include "Tool.h"

#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/iterator"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/System/Path.h"

#include <string>

namespace llvmcc {

  class CompilationGraph;

  struct Node {
    typedef llvm::SmallVector<std::string, 3> sequence_type;

    Node() {}
    Node(CompilationGraph* G) : OwningGraph(G) {}
    Node(CompilationGraph* G, Tool* T) : OwningGraph(G), ToolPtr(T) {}

    // Needed to implement NodeChildIterator/GraphTraits
    CompilationGraph* OwningGraph;
    // The corresponding Tool.
    llvm::IntrusiveRefCntPtr<Tool> ToolPtr;
    // Links to children.
    sequence_type Children;
  };

  // This can be generalised to something like value_iterator for maps
  class NodesIterator : public llvm::StringMap<Node>::iterator {
    typedef llvm::StringMap<Node>::iterator super;
    typedef NodesIterator ThisType;
    typedef Node* pointer;
    typedef Node& reference;

  public:
    NodesIterator(super I) : super(I) {}

    inline reference operator*() const {
      return super::operator->()->second;
    }
    inline pointer operator->() const {
      return &super::operator->()->second;
    }
  };

  class CompilationGraph {
    typedef llvm::StringMap<Node> nodes_map_type;
    typedef llvm::SmallVector<std::string, 3> tools_vector_type;
    typedef llvm::StringMap<tools_vector_type> tools_map_type;

    // Map from file extensions to language names.
    LanguageMap ExtsToLangs;
    // Map from language names to lists of tool names.
    tools_map_type ToolsMap;
    // Map from tool names to Tool objects.
    nodes_map_type NodesMap;

  public:

    CompilationGraph();

    // insertVertex - insert a new node into the graph.
    void insertVertex(const llvm::IntrusiveRefCntPtr<Tool> T);

    // insertEdge - Insert a new edge into the graph. This function
    // assumes that both A and B have been already inserted.
    void insertEdge(const std::string& A, const std::string& B);

    // Build - Build the target(s) from the set of the input
    // files. Command-line options are passed implicitly as global
    // variables.
    int Build(llvm::sys::Path const& tempDir) const;

    /// viewGraph - This function is meant for use from the debugger.
    /// You can just say 'call G->viewGraph()' and a ghostview window
    /// should pop up from the program, displaying the compilation
    /// graph. This depends on there being a 'dot' and 'gv' program
    /// in your path.
    void viewGraph();

    /// Write a CompilationGraph.dot file.
    void writeGraph();

    // GraphTraits support

    typedef NodesIterator nodes_iterator;

    nodes_iterator nodes_begin() {
      return NodesIterator(NodesMap.begin());
    }

    nodes_iterator nodes_end() {
      return NodesIterator(NodesMap.end());
    }

    // Return a reference to the node correponding to the given tool
    // name. Throws std::runtime_error in case of error.
    Node& getNode(const std::string& ToolName);
    const Node& getNode(const std::string& ToolName) const;

    // Auto-generated function.
    friend void PopulateCompilationGraph(CompilationGraph&);

  private:
    // Helper function - find out which language corresponds to the
    // suffix of this file
    const std::string& getLanguage(const llvm::sys::Path& File) const;

    // Return a reference to the tool names list correponding to the
    // given language name. Throws std::runtime_error in case of
    // error.
    const tools_vector_type& getToolsVector(const std::string& LangName) const;
  };

  // Auxiliary class needed to implement GraphTraits support.
  class NodeChildIterator : public bidirectional_iterator<Node, ptrdiff_t> {
    typedef NodeChildIterator ThisType;
    typedef Node::sequence_type::iterator iterator;

    CompilationGraph* OwningGraph;
    iterator KeyIter;
  public:
    typedef Node* pointer;
    typedef Node& reference;

    NodeChildIterator(Node* N, iterator I) :
      OwningGraph(N->OwningGraph), KeyIter(I) {}

    const ThisType& operator=(const ThisType& I) {
      assert(OwningGraph == I.OwningGraph);
      KeyIter = I.KeyIter;
      return *this;
    }

    inline bool operator==(const ThisType& I) const
    { return KeyIter == I.KeyIter; }
    inline bool operator!=(const ThisType& I) const
    { return KeyIter != I.KeyIter; }

    inline pointer operator*() const {
      return &OwningGraph->getNode(*KeyIter);
    }
    inline pointer operator->() const {
      return &OwningGraph->getNode(*KeyIter);
    }

    ThisType& operator++() { ++KeyIter; return *this; } // Preincrement
    ThisType operator++(int) { // Postincrement
      ThisType tmp = *this;
      ++*this;
      return tmp;
    }

    inline ThisType& operator--() { --KeyIter; return *this; }  // Predecrement
    inline ThisType operator--(int) { // Postdecrement
      ThisType tmp = *this;
      --*this;
      return tmp;
    }

  };
}

namespace llvm {
  template <>
  struct GraphTraits<llvmcc::CompilationGraph*> {
    typedef llvmcc::CompilationGraph GraphType;
    typedef llvmcc::Node NodeType;
    typedef llvmcc::NodeChildIterator ChildIteratorType;

    static NodeType* getEntryNode(GraphType* G) {
      return &G->getNode("root");
    }

    static ChildIteratorType child_begin(NodeType* N) {
      return ChildIteratorType(N, N->Children.begin());
    }
    static ChildIteratorType child_end(NodeType* N) {
      return ChildIteratorType(N, N->Children.end());
    }

    typedef GraphType::nodes_iterator nodes_iterator;
    static nodes_iterator nodes_begin(GraphType *G) {
      return G->nodes_begin();
    }
    static nodes_iterator nodes_end(GraphType *G) {
      return G->nodes_end();
    }
  };

}

#endif // LLVM_TOOLS_LLVMC2_COMPILATION_GRAPH_H
