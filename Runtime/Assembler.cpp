#include <vector>
#include <string.h>
#include <sys/mman.h>
#include <stdint.h>
#include "Runtime.h"
class Assembler {
public:
	//Creates an external function call
	virtual void Call(void* ptr, void* args, size_t len) = 0; //Invokes a function, specified by a given pointer
	virtual void* Jit() = 0;
	virtual void Unjit(void* ptr) = 0;
};

class x86Assembler:Assembler {
public:
	void append(unsigned char* bytes, size_t len) {
		size_t pos = assembly.size();
		assembly.resize(assembly.size() + len);
		memcpy(assembly.data() + pos, bytes, len);
	}
	std::vector<unsigned char> assembly;
	WindowsAssembler() {

	}


	//MOD = 0b11 for register direct addressing, else 0 for register indirect
	//O = secondary OPCODE differentiator or register identifier
	//rm = Second register operand, or 3-bit OPCODE extension
	uint64_t encode_modrm(uint64_t mod, uint64_t o, uint64_t rm) {
		return (mod << 6) + (o << 3) + rm;
	}
	//S == 2-bit scale, factors go (b00 = 1, b01=2, b10=4, b11=8)
	//I == Index register to use
	//B == Base register to use
	//NOTE: Address = (B+(I*S))
	//IMPORTANT: Odd-numbered addresses may not work in protected mode if the A20 line is disabled.
	uint64_t encode_sib(uint64_t s, uint64_t i, uint64_t b) {
		return encode_modrm(s, i, b);
	}
	//Copy from src to dest register
	void RegCopy(unsigned char dest, unsigned char src) {
		assembly.push_back(0b01001000); //REX prefix (64-bit mode)
		assembly.push_back(0x89); //MOVE
		assembly.push_back(encode_modrm(0b11, src, dest));

	}
	//Store data in srcReg to mem[addrReg]
	void Store(unsigned char srcReg, unsigned char addrReg) {
		assembly.push_back(0b01001000);
		assembly.push_back(0x89);
		assembly.push_back(encode_modrm(0, srcReg, addrReg));
	}

	void LoadImmediate(uint64_t value, unsigned char reg) {
		unsigned char data[1 + 9];
		data[0] = 0x48;
		data[1] = 0xb8 + reg;
		memcpy(data + 2, &value, 8);
		append(data, 10);
	}
	

	void Push(unsigned char reg) {
		assembly.push_back(0x50+reg);
	}
	//Pop a value into the RDI register
	void Pop(unsigned char reg) {
		assembly.push_back(0x58+reg);
	}
	
	void CallNative(unsigned char reg) {
		assembly.push_back(0xff); //Base instruction
		assembly.push_back(encode_modrm(0b11, 2,reg)); //ModRM formatted sub-instruction, with differentiator code 2, and user-defined register

	}

	void Call(void* ptr, void* args, size_t len) {
		//Long jump, make full word
		Push(0);
		LoadImmediate((size_t)ptr, 0);
		CallNative(0);
		Pop(0);
	}
	void Unjit(void* ptr) {
	  uint64_t* code = (uint64_t*)ptr;
	  mprotect(code+1,*code, PROT_READ | PROT_WRITE);
	  delete[] code;
	  
	  
	}
	void* Jit() {
		//Add RET instruction (195, make sure to add enough C3)
		assembly.push_back(195);
		uint64_t* code = new uint64_t[(assembly.size()/8)+2];
		code[0] = (uint64_t)assembly.size();
		memcpy(code+1,assembly.data(),assembly.size());
		mprotect(code+1,assembly.size(),PROT_READ | PROT_EXEC);
		assembly.resize(0);
		return code;
	}
};

extern "C" {
  void* ASM_ModuleInit() {
    return new x86Assembler();
    
  }
  void* ASM_ModuleJit(void* module) {
       Assembler* a = (Assembler*)module;
       std::vector<unsigned char> assembly;
       a->Jit(assembly);
      
  }
  void ASM_ModuleFree(void* module, void* compiledModule) {
    Assembler* a = (Assembler*)module;
    a->Unjit(compiledModule);
  }
  void ASM_ModuleCall(void* module, void* address, void* args, size_t len) {
    Assembler* a = (Assembler*)module;
    a->Call(address,args,len);
  }
  
}
