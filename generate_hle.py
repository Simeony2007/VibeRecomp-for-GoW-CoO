import json

def build_hle_router():
    print("[HLE Builder] Lendo elf_meta.json...")
    
    try:
        with open("elf_meta.json", "r") as f:
            meta = json.load(f)
            
        imports = meta.get("hle_imports", [])
        
        with open("hle_router.h", "w") as out:
            out.write("/* Roteador HLE Definitivo (Gerado Nativamente) */\n")
            out.write("#pragma once\n")
            out.write("#include <stdint.h>\n")
            out.write("#include <string.h>\n\n")
            out.write("extern void real_hle_call(MIPS_CPU *cpu, uint8_t *mem, const char *name);\n\n")
            out.write("static inline int intercept_hle(MIPS_CPU *cpu, uint8_t *mem, uint32_t target) {\n")
            out.write("    switch(target) {\n")
            
            for imp in imports:
                # Gera as rotas exatas, conectadas ao real_hle_call!
                out.write(f'        case {imp["stub_addr"]}: real_hle_call(cpu, mem, "{imp["name"]}"); return 1;\n')
                
            out.write("    }\n")
            out.write("    return 0;\n")
            out.write("}\n")
            
        print(f"[HLE Builder] ✅ hle_router.h gerado com {len(imports)} funções nativas!")
        
    except FileNotFoundError:
        print("[ERRO] elf_meta.json não encontrado. Rode o parse_elf.py primeiro!")

if __name__ == "__main__":
    build_hle_router()