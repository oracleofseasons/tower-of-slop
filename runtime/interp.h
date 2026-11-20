// GML interpreter: the game's code is read from the pak at load and run from a resolved syntax tree.
// Lexing, parsing and name resolution mirror tools/transpiler/gml2cpp.py exactly (which mirrors
// OpenGMK's gml-parser), so the interpreter behaves like the transpiled build it replaces.
#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gml_runtime.h"

namespace gml {

struct Node;

struct Code {
    std::string desc;
    std::vector<std::unique_ptr<Node>> nodes;   // owns every node of the piece
    Node* root = nullptr;
    int nlocals = 0;
    Code();
    ~Code();   // in interp.cpp, where Node is complete
};

class Interp {
  public:
    // names known before code is resolved
    std::unordered_map<std::string, int> assets;       // asset name -> id (all kinds, as GM's namespace)
    std::unordered_map<std::string, int> script_ids;   // script name -> id

    // user variables: ids are allocated on first use
    std::vector<std::string> var_names;
    int var_id(const std::string& name);
    int find_var(const std::string& name) const;       // -1 if no code uses it

    // 1. parse every piece (throws GmlError with the piece's description)
    Code* parse(const std::string& src, const std::string& desc);
    // 2. after all pieces are parsed: resolve names (needs every piece's globalvar declarations)
    void link();
    void set_script(int id, const Code* code);

    Value run(const Code* code, Ctx& C);

  private:
    std::vector<std::unique_ptr<Code>> codes;
    std::vector<const Code*> scripts;                  // by script id
    std::unordered_set<std::string> globalvars;
    std::unordered_map<std::string, int> var_ids;
    void resolve(Code& code);
    friend struct Resolver;
    friend struct Exec;
};

}  // namespace gml
