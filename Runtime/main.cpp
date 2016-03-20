#include <stdio.h>
#include <map>
#include <string.h>
#include <memory>
#include <stack>
#include <vector>
#include <complex>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <set>

void* gc;




class BStream {
public:
  unsigned char* ptr;
  size_t len;
  BStream() {
  }
  BStream(void* ptr, size_t sz) {
    this->ptr = (unsigned char*)ptr;
    this->len = sz;
  }
  void Read(void* output, size_t len) {
    if(len>this->len) {
      throw "counterclockwise"; //How is this even possible?
    }
    
    memcpy(output,ptr,len);
    this->len-=len;
    this->ptr+=len;
  }
  template<typename T>
  T& Read(T& val) {
    if(sizeof(val)>len) {
      throw "down";
    }
    memcpy(&val,ptr,sizeof(val));
    this->len-=sizeof(val);
    this->ptr+=sizeof(val);
    return val;
  }
  void* Increment(size_t len) {
    void* retval = this->ptr;
    if(len>this->len) {
      throw "Increment -- Read past end of file"; //barf
    }
    this->len-=len;
    this->ptr+=len; 
    return retval;
  }
  char* ReadString() {
    char* start = (char*)this->ptr;
    size_t len = strnlen((char*)this->ptr,this->len)+1;
    if(len>this->len) {
      throw "away your assembly code. Trust me. It's NO good!";
    }
    this->ptr+=len;
    this->len-=len;
    return start;
  }
  
};


class UALModule; //forward-declaration



/**
 * @summary Reads white space
 * @param found True if whitespace was found, false otherwise
 * @returns The string up to the whitespace
 * */
static std::string Parser_ExpectWhitespace(const char*& str, bool& found) {
  std::string retval; //std::string is slow. We could optimize this; but assembly loading/JITting or interpreting is expected to be slow at initial load with most runtimes. Performance shouldn't matter too much at early loading stage.
  found = false;
  while(*str != 0) {
    if(isspace(*str)) {
      while(isspace(*str)){str++;}
      found = true;
      return retval;
    }else {
      retval+=*str;
    }
    str++;
  }
  return retval;
}

/**
 * @summary Expects a specific character
 * @param acter The character to expect
 * @param found True if the specified character was found, false otherwise.
 * @returns The text up to the specified character
 * */
static std::string Parser_ExpectChar(const char*& str, char acter, bool& found) {
  std::string retval;
  while(*str != 0) {
    if(*str == acter) {
      str++;
      return retval;
    }
    retval+=*str;
    str++;
  }
  return retval;
}


/**
 * @summary Expects a list of characters
 * @param acters A list of characters to search for, followed by a NULL terminator
 * @param found The memory address of the character which was found, or NULL if the character was not found.
 * @returns The text up to the specified character
 * */
static std::string Parser_ExpectMultiChar(const char*& str, const char* acters, const char*& found) {
  std::string retval;
  found = 0;
  while(*str != 0) {
    const char* current = acters;
    while(*current != 0) {
    if(*str == *current) {
      found = str;
      str++;
      return retval;
    }
    current++;
    }
    retval+=*str;
    str++;
  }
  return retval;
}

/**
 * @summary Expects a string
 * @param acter The string to expect
 * @param _found True if the specified string was found, false otherwise.
 * @returns The text up to the specified string
 * */
static std::string Parser_ExpectString(const char*& str, const char* acter, bool& found) {
  std::string retval;
  while(*str != 0) {
    if(*str == acter[0]) {
      const char* cpos = acter;
      found = true;
      while(*str != 0) {
	if(*cpos == 0) {
	  //Found whole string
	  found = true;
	  return retval;
	}
	if(*str != *cpos) {
	  found = false;
	  break;
	}
	str++;
	cpos++;
      }
      str++;
      return retval;
    }
    retval+=*str;
    str++;
  }
  return retval;
}


/**
 * Contains information about a method signature
 * */
class MethodSignature {
public:
  std::string fullSignature;
  std::string returnType; //Return type of method
  std::string className; //Fully-qualified class name of method
  std::string methodName;
  std::vector<std::string> args;
  MethodSignature() {
  }
  MethodSignature(const char* rawName) {
    fullSignature = rawName;
    bool found;
   returnType = Parser_ExpectWhitespace(rawName,found);
   className = Parser_ExpectString(rawName,"::",found);
   methodName = Parser_ExpectChar(rawName,'(',found);
   
   while(true) {
     const char* foundChar;
     std::string argType = Parser_ExpectMultiChar(rawName,",)",foundChar);
     if(argType.size()) {
      args.push_back(argType);
     }
     if(*foundChar == ')') {
       //End of arguments
       break;
     }
   }
  }
  bool operator<(const MethodSignature& other) const {
    return other.fullSignature<fullSignature;
  }
};






class Type {
public:
  size_t size; //The total size of this type (used when allocating memory)
  std::map<std::string,Type*> fields; //Fields in this type
  std::string name; //The fully-qualified name of the type
  bool isStruct; //Whether or not this type should be treated as a struct or a managed object.
  virtual ~Type(){};
};


class UALMethod;
static UALMethod* ResolveMethod(void* assembly, uint32_t handle);


Type* ResolveType(const char* name);


//A parse tree node

enum NodeType {
  NCallNode, NConstantInt, NConstantDouble, NConstantString, NLdLoc, NStLoc, NLdArg, NRet, NBranch, NBinaryExpression, NOPE
};
class Node {
public:
  NodeType type;
  std::string resultType;
  Node* next;
  Node* prev;
  bool fpEmit; //Whether or not the expression's output should be saved to the floating point stack.
  bool bound;
  Node(NodeType type) {
    this->type = type;
    this->next = 0;
    this->prev = 0;
    this->fpEmit = false;
    bound = false;
  }
  virtual ~Node(){};
};

class ConstantInt:public Node {
public:
  uint32_t value;
  ConstantInt(uint32_t val):Node(NodeType::NConstantInt) {
    value = val;
    resultType = "System.Int32";
  }
};
class ConstantDouble:public Node {
public:
  double value;
  ConstantDouble(double val):Node(NodeType::NConstantDouble) {
    this->value = val;
    resultType = "System.Double";
  }
};

class ConstantString:public Node {
public:
  const char* value;
  ConstantString(const char* val):Node(NodeType::NConstantString) {
    this->value = val;
    resultType = "System.String";
  }
};

class LdLoc:public Node {
public:
  size_t idx; //The index of the local field to load from
  LdLoc(size_t idx, const char* type):Node(NodeType::NLdLoc) {
    this->idx = idx;
    this->resultType = type;
  }
};
class StLoc:public Node {
public:
  size_t idx;
  Node* exp; //The expression to store into the variable.
  StLoc(size_t idx, Node* exp):Node(NodeType::NStLoc) {
    this->idx = idx;
    this->exp = exp;
  }
};

class LdArg:public Node {
public:
  size_t index;
  LdArg(size_t index, const char* type):Node(NodeType::NLdArg) {
    this->index = index;
    this->resultType = type;
  }
};

class Ret:public Node {
public:
  Node* resultExpression;
  Ret(Node* resultExpression):Node(NRet) {
    this->resultExpression = resultExpression;
  }
};
enum BranchCondition {
  UnconditionalSurrender, //Complete and unconditional branch.
  Ble, //Barf
  Blt, //Bacon, Lettuce, and Tomato
  Bgt, //Bacon, Gravy, and Tomato.,
  Bge, //Branch on greater than or equal to
  Beq, //Branch on equal
  Bne //Branch on not equal
  
};
class Branch:public Node {
public:
  Node* left; //Democratic operand on comparison
  Node* right; //Republican operand on comparison
  uint32_t offset; //Byte offset from start of executable segment of method.
  BranchCondition condition;
  Branch(uint32_t ualoffset, BranchCondition condition, Node* democrat, Node* republican):Node(NBranch) {
    offset = ualoffset;
    this->condition = condition;
    left = democrat;
    right = republican;
  }
};



//Objects are referred to when possible by reference identifier strings
//to allow for dynamic modules to be loaded and unloaded without requiring recompilation of existing code,
//and to allow for recompilation of a method if necessary for whatever reason.
class CallNode:public Node {
public:
  UALMethod* method;
  std::vector<Node*> arguments;
  CallNode(UALMethod* method, const std::vector<Node*>& arguments):Node(NodeType::NCallNode) {
    this->method = method;
    this->arguments = arguments;
  }
  
};


class BinaryExpression:public Node {
public:
  char op;
  //OPS
  //+ -- Addition
  //- -- Subtraction
  //* -- Multiplication
  /// -- Division
  //% -- Reminder
  //< -- Shift left
  //> -- Shift right
  //& -- AND
  //| -- OR
  //! -- NOT (only needs Democratic debate)
  //~ -- XOR
  Node* left;
  Node* right;
  BinaryExpression(char op, Node* democrat, Node* republican):Node(NBinaryExpression) {
    this->op = op;
    left = democrat;
    right = republican;
    this->resultType = left->resultType;
  }
};



class UALMethod {
public:
  BStream str;
  bool isManaged;
  void* assembly;
  MethodSignature sig;
  uint32_t localVarCount;
  std::vector<std::string> locals;
  
  //BEGIN Optimization engine:
  std::vector<Node*> nodes;
    std::vector<Node*> stack;
  Node* instructions;
  Node* lastInstruction;
  std::map<uint32_t,Node*> ualOffsets;
  template<typename T, typename... arg>
  //Adds an Instruction node to the tree
  T* Node_Instruction(arg... uments) {
    T* retval = new T(uments...);
    nodes.push_back(retval);
    if(instructions == 0) {
      instructions = retval;
      lastInstruction = retval;
    }else {
      lastInstruction->next = retval;
      retval->prev = lastInstruction;
      lastInstruction = retval;
    }
    ualOffsets[this->ualip] = retval;
    return retval;
  }
  template<typename T, typename... arg>
  T* Node_Stackop(arg... uments) {
    T* retval = new T(uments...);
    nodes.push_back(retval);
    stack.push_back(retval);
    ualOffsets[this->ualip] = retval;
    return retval;
  }
  uint32_t ualip;
  template<typename T>
  //Removes an instruction node
  T* Node_RemoveInstruction(T* node) {
    if(node->prev) {
      node->prev->next = node->next;
    }
    if(node->next) {
      node->next->prev = node->prev;
    }
    if(node == instructions) {
      instructions = node->next;
    }
    if(node == lastInstruction) {
      lastInstruction = node->prev;
    }
    
    node->next = 0;
    node->prev = 0;
    return node;
  }
  //END Optimization engine
  
  
  
  UALMethod(const BStream& str, void* assembly, const char* sig) {
    this->instructions = 0;
    this->sig = sig;
    this->str = str;
    this->str.Read(isManaged);
    ualip = 0;
    if(isManaged) {
      this->str.Read(localVarCount);
      locals.resize(localVarCount);
      for(size_t i = 0;i<localVarCount;i++) {
	locals[i] = this->str.ReadString();
      }
    }
    this->assembly = assembly;
    
  }
  

  
  
  void Optimize() {
  }
  void Emit() {
  
  }
  void Compile() {
    Parse();
    Optimize();
    Emit();
  }
  
  
  void Parse() {
    
    //Generate parse tree
    unsigned char opcode;
    unsigned char* base = str.ptr;
    BStream reader = str;
    while(reader.Read(opcode) != 255) {
      ualip = (uint32_t)((size_t)reader.ptr-(size_t)base)-1;
      
      printf("OPCODE: %i\n",(int)opcode);
      switch(opcode) {
	case 0:
	{
	  //Push argument to evaluation stack
	  uint32_t index;
	  reader.Read(index);
	  std::string tname = sig.args[index];
	  Node* sobj = Node_Stackop<LdArg>(index,tname.data());
	  
	  
	}
	  break;
	case 1:
	  //Call function
	{
	  uint32_t funcID;
	  reader.Read(funcID);
	  UALMethod* method = ResolveMethod(assembly,funcID);
	  size_t argcount = method->sig.args.size();
	  std::vector<Node*> args;
	  args.resize(argcount);
	  for(size_t i = 0;i<argcount;i++) {
	    if(stack.size() == 0) {
	      throw "Malformed UAL. Too few arguments in function call.";
	    }
	    args[i] = stack[stack.size()-1];
	    if(args[i]->resultType != method->sig.args[i]) {
	      throw "Malformed UAL. Illegal data type passed to function.";
	    }
	    stack.pop_back();
	    Node_RemoveInstruction(args[i]);
	  }
	  Node* sobj = Node_Instruction<CallNode>(method,args);
	  
	  
	}
	  break;
	case 2:
	  //Load string
	{
	  Node* sobj = Node_Stackop<ConstantString>(reader.ReadString());
	  
	}
	  break;
	case 3:
	{
	  if(this->sig.returnType == "System.Void") {
	    //There should be nothing on stack
	    if(stack.size()) {
	      throw "Malformed UAL. Function should not return a value.";
	    }
	  }else {
	      if(stack.size() != 1) {
		throw "Malformed UAL. Function must return a value.";
	      }
	      if(stack[0]->resultType != this->sig.returnType) {
		throw "Malformed UAL. Function does not return correct datatype.";
	      }
	      Node_Instruction<Ret>(Node_RemoveInstruction(stack[0]));
	      
	    }
	}
	  break;
	case 4:
	{
	  //Load 32-bit integer immediate
	  uint32_t val;
	  reader.Read(val);
	  Node_Stackop<ConstantInt>(val);
	}
	  break;
	case 5:
	  //stloc
	{
	  uint32_t index;
	  reader.Read(index);
	  
	  //Store value to local
	  if(stack.size() == 0) {
	    throw "Malformed UAL. Expected value on stack.";
	  }
	  Node* sval = stack[stack.size()-1];
	  stack.pop_back();
	  if(sval->resultType != this->locals[index]) {
	    printf("Type mismatch %s != %s\n",sval->resultType.data(),this->locals[index].data());
	    throw "Malformed UAL. Type mismatch on store to local variable.";
	  }
	  Node_Instruction<StLoc>(index,Node_RemoveInstruction(sval));
	  
	  
	  
	}
	  break;
	case 6:
	  //Branch to str.ptr+offset
	{
	  uint32_t offset;
	  reader.Read(offset);
	   Node_Instruction<Branch>(offset, UnconditionalSurrender, (Node*)0, (Node*)0);
	   
	}
	  break;
	case 7:
	{
	  //LDLOC
	  uint32_t id;
	  reader.Read(id);
	  std::string tname = this->locals[id];
	  Node_Stackop<LdLoc>(id,tname.data());
	}
	  break;
	case 8:
	{
	  if(stack.size() < 2) {
	    throw "Malformed UAL. Expected two operands on stack.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Binary expressions require operands to be of same type.";
	  }
	  if(!(left->resultType == "System.Double" || left->resultType == "System.Int32")) {
	    throw "Malformed UAL. Binary expressions can only operate on primitive types.";
	  }
	  Node_Stackop<BinaryExpression>('+',Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	  
	}
	  break;
	case 9:
	{
	  //BLE!!!!!! (NOTE: emit barfing sound here into the assembly code)
	  uint32_t offset;
	  reader.Read(offset);
	  if(stack.size()<2) {
	    throw "Malformed UAL. Comparison expressions must use BOTH a Democrat and a Republican.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Type mismatch in conditional branch.";
	  }
	  Node_Instruction<Branch>(offset,Ble,Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	}
	  break;
	case 10:
	  //NOPE. Not gonna happen.
	{
	  Node_Instruction<Node>(NOPE);
	}
	  break;
	case 11:
	  {
	  //BEQ
	  uint32_t offset;
	  reader.Read(offset);
	  if(stack.size()<2) {
	    throw "Malformed UAL. Comparison expressions must use BOTH a Democrat and a Republican.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Type mismatch in conditional branch.";
	  }
	  Node_Instruction<Branch>(offset,Beq,Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	}
	break;
	case 12:
	{
	  //BNE
	  uint32_t offset;
	  reader.Read(offset);
	  if(stack.size()<2) {
	    throw "Malformed UAL. Comparison expressions must use BOTH a Democrat and a Republican.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Type mismatch in conditional branch.";
	  }
	  Node_Instruction<Branch>(offset,Bne,Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	}
	  break;
	  case 13:
	{
	  //BGT
	  uint32_t offset;
	  reader.Read(offset);
	  if(stack.size()<2) {
	    throw "Malformed UAL. Comparison expressions must use BOTH a Democrat and a Republican.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Type mismatch in conditional branch.";
	  }
	  Node_Instruction<Branch>(offset,Bgt,Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	  
	}
	  break;
	  case 14:
	{
	  //>=
	  uint32_t offset;
	  reader.Read(offset);
	  if(stack.size()<2) {
	    throw "Malformed UAL. Comparison expressions must use BOTH a Democrat and a Republican.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Type mismatch in conditional branch.";
	  }
	  Node_Instruction<Branch>(offset,Bge,Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	}
	  break;
	  case 15:
	{
	  
	  
	  if(stack.size() < 2) {
	    throw "Malformed UAL. Expected two operands on stack.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Binary expressions require operands to be of same type.";
	  }
	  if(!(left->resultType == "System.Double" || left->resultType == "System.Int32")) {
	    throw "Malformed UAL. Binary expressions can only operate on primitive types.";
	  }
	  Node_Stackop<BinaryExpression>('-',Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	  
	  
	}
	  break;
	  case 16:
	{
	  if(stack.size() < 2) {
	    throw "Malformed UAL. Expected two operands on stack.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Binary expressions require operands to be of same type.";
	  }
	  if(!(left->resultType == "System.Double" || left->resultType == "System.Int32")) {
	    throw "Malformed UAL. Binary expressions can only operate on primitive types.";
	  }
	  Node_Stackop<BinaryExpression>('*',Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	}
	  break;
	  case 17:
	{
	  if(stack.size() < 2) {
	    throw "Malformed UAL. Expected two operands on stack.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Binary expressions require operands to be of same type.";
	  }
	  if(!(left->resultType == "System.Double" || left->resultType == "System.Int32")) {
	    throw "Malformed UAL. Binary expressions can only operate on primitive types.";
	  }
	  Node_Stackop<BinaryExpression>('/',Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	  
	}
	  break;
	  case 18:
	{
	  if(stack.size() < 2) {
	    throw "Malformed UAL. Expected two operands on stack.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Binary expressions require operands to be of same type.";
	  }
	  if(!(left->resultType == "System.Int32")) {
	    throw "Malformed UAL. Binary expressions can only operate on primitive types.";
	  }
	  Node_Stackop<BinaryExpression>('%',Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	  
	}
	  break;
	  
	    case 19:
	{
	  
	  if(stack.size() < 2) {
	    throw "Malformed UAL. Expected two operands on stack.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Binary expressions require operands to be of same type.";
	  }
	  if(!(left->resultType == "System.Int32")) {
	    throw "Malformed UAL. Binary expressions can only operate on primitive types.";
	  }
	  Node_Stackop<BinaryExpression>('<',Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	  
	  
	  
	}
	break;
	
	    case 20:
	{
	  
	  if(stack.size() < 2) {
	    throw "Malformed UAL. Expected two operands on stack.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Binary expressions require operands to be of same type.";
	  }
	  if(!(left->resultType == "System.Int32")) {
	    throw "Malformed UAL. Binary expressions can only operate on primitive types.";
	  }
	  Node_Stackop<BinaryExpression>('>',Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	  
	}
	break;  
	
	    case 21:
	{
	  
	  if(stack.size() < 2) {
	    throw "Malformed UAL. Expected two operands on stack.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Binary expressions require operands to be of same type.";
	  }
	  if(!(left->resultType == "System.Int32")) {
	    throw "Malformed UAL. Binary expressions can only operate on primitive types.";
	  }
	  Node_Stackop<BinaryExpression>('&',Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	}
	break;  
	
	    case 22:
	{
	  
	  if(stack.size() < 2) {
	    throw "Malformed UAL. Expected two operands on stack.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Binary expressions require operands to be of same type.";
	  }
	  if(!(left->resultType == "System.Int32")) {
	    throw "Malformed UAL. Binary expressions can only operate on primitive types.";
	  }
	  Node_Stackop<BinaryExpression>('|',Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	}
	break;  
	
	    case 23:
	{
	  
	  if(stack.size() < 2) {
	    throw "Malformed UAL. Expected two operands on stack.";
	  }
	  Node* left = stack[stack.size()-1];
	  stack.pop_back();
	  Node* right = stack[stack.size()-1];
	  stack.pop_back();
	  if(left->resultType != right->resultType) {
	    throw "Malformed UAL. Binary expressions require operands to be of same type.";
	  }
	  if(!(left->resultType == "System.Int32")) {
	    throw "Malformed UAL. Binary expressions can only operate on primitive types.";
	  }
	  Node_Stackop<BinaryExpression>('~',Node_RemoveInstruction(left),Node_RemoveInstruction(right));
	  
	}
	break;  
	
	    case 24:
	{
	  
	  if(stack.size() < 1) {
	    throw "Malformed UAL. Expected at least one operand on the stack.";
	  }
	  Node* left = stack[stack.size()-1];
	  if(!(left->resultType == "System.Int32")) {
	    throw "Malformed UAL. Binary expressions can only operate on primitive types.";
	  }
	  Node_Stackop<BinaryExpression>('!',Node_RemoveInstruction(left),(Node*)0);
	  
	  
	}
	break;  
	    case 25:
	    {
	      //Load FP immediate
	      double word; //We read in a doubleword
	      reader.Read(word);
	      Node_Stackop<ConstantDouble>(word);
	      
	    }
	      break;
	
	default:
	  printf("Unknown OPCODE %i\n",(int)opcode);
	  goto velociraptor;
      }
    }
    
    velociraptor: //Back pain? Visit your GOTO Velociraptor today!
    return;
    
  }

  ~UALMethod() {
    size_t l = nodes.size();
    for(size_t i = 0;i<l;i++) {
      delete nodes[i];
    }
    
  }
}; 

static std::map<std::string,UALMethod*> methodCache;
class UALType:public Type {
public:
  
  BStream bstr; //in-memory view of file
  bool compiled; //Whether or not this module has been compiled or interpreted
  UALModule* module;
  std::map<std::string,UALMethod*> methods;
  UALType(BStream& str, UALModule* module) {
    bstr = str;
    compiled = false;
    this->module = module;
    
  }
  UALType() {
    //Special case: Builtin type.
    compiled = true;
  }
  /**
   * @summary Compiles this UAL type to native code (x86), or interprets
   * */
  void Compile() {
    if(!compiled) {
    uint32_t count;
    bstr.Read(count);
    size_t nativeCount = count; //Copy to size_t for faster performance
    for(size_t i = 0;i<nativeCount;i++) {
      const char* mname = bstr.ReadString();
      uint32_t mlen;
      bstr.Read(mlen);
      
      void* ptr = bstr.Increment(mlen);
      UALMethod* method = new UALMethod(BStream(ptr,mlen),module,mname);
      method->sig = mname;
      methods[mname] = method;
      methodCache[mname] = method;
    }
    }
  }
};


static std::map<std::string,Type*> typeCache; //Cache of types
Type* ResolveType(const char* name)
{
  return typeCache[name];
}

class UALModule {
public:
  std::map<std::string,UALType*> types;
  std::map<uint32_t,std::string> methodImports;
  
  UALModule(void* bytecode, size_t len) {
    BStream str(bytecode,len);
    uint32_t count;
    str.Read(count);
#ifdef DEBUGMODE
    printf("Reading in %i classes\n",(int)count);
#endif

    for(uint32_t i = 0;i<count;i++) {
      char* name = str.ReadString();
      uint32_t asmlen;
      str.Read(asmlen);
      
      BStream obj(str.Increment(asmlen),asmlen);
      #ifdef DEBUGMODE
      printf("Loading %s\n",name);
      #endif
      UALType* type = new UALType(obj,this);

      types[std::string(name)] = type;
      typeCache[std::string(name)] = type;

    }

    str.Read(count);
    printf("Reading %i methods\n",count);

    for(uint32_t i = 0;i<count;i++) {
      uint32_t id;
      str.Read(id);
      char* methodName = str.ReadString();
      methodImports[id] = methodName;
      printf("Found %s\n",methodName);
    }
    
  }
  void LoadMain(int argc, char** argv) {
    //Find main
    //TODO: Parse method signatures from strings
      UALType* mainClass = 0;
      UALMethod* mainMethod = 0;
      for(auto i = types.begin();i!= types.end();i++) {
	i->second->Compile();
	for(auto bot = i->second->methods.begin();bot != i->second->methods.end();bot++) {
	  MethodSignature sig(bot->first.c_str());
	  if(sig.methodName == "Main" && sig.args.size() == 1) {
	    if(sig.args[0] == "System.String[]") {
	      mainMethod = bot->second;
	      mainClass = i->second;
	    }
	  }
	}
	
      }
      if(mainClass == 0) {
	printf("Error: Unable to find Main.\n");
	return;
      }
  }
};


static UALMethod* ResolveMethod(void* assembly, uint32_t handle) {
  UALModule* module = (UALModule*)assembly;
  std::string signature = module->methodImports[handle];
  if(methodCache.find(signature) == methodCache.end()) {
    return 0;
  }
  return methodCache[signature];
}

void Print(const char* txt);
void RunModule(int fd, int offset) {
    try {


        UALType *btype = new UALType();
        btype->isStruct = true;
        btype->size = 4; //32-bit integer.
        btype->name = "System.Int32";
        typeCache["System.Int32"] = btype;
        btype = new UALType();
        btype->isStruct = false;
        btype->size = sizeof(size_t); //A String just has a single pointer.
        btype->name = "System.String";
        typeCache["System.String"] = btype;
        btype = new UALType();
        btype->isStruct = true;
        btype->size = 8;
        btype->name = "System.Double";
        typeCache["System.Double"] = btype;


        struct stat us; //It's a MAC (status symbol)
        fstat(fd, &us);
        size_t len = us.st_size;
        void *ptr = mmap(0, len, PROT_READ, MAP_SHARED, fd, 0);

        UALModule *module = new UALModule(ptr+offset, len-offset);
    }catch(const char* er) {
        Print(er);
    }
}
