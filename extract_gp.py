#!/usr/bin/env python3
"""
Script para extrair o GP (_gp / __gnu_local_gp) correto do ELF.
"""

import struct
import sys
import json

def extract_gp_from_elf(elf_path):
    """Extrai o GP lendo a seção de símbolos do ELF."""
    
    try:
        with open(elf_path, 'rb') as f:
            elf_data = f.read()
    except FileNotFoundError:
        print(f"[ERRO] Arquivo {elf_path} não encontrado")
        return None
    
    # Verificar magic number ELF
    if elf_data[:4] != b'\x7fELF':
        print("[ERRO] Arquivo não é um ELF válido")
        return None
    
    # Ler header
    e_shoff = struct.unpack('<I', elf_data[32:36])[0]  # Seção headers offset
    e_shnum = struct.unpack('<H', elf_data[48:50])[0]  # Número de seções
    e_shstrndx = struct.unpack('<H', elf_data[50:52])[0]  # Índice string table
    
    print(f"[INFO] ELF Header: shoff=0x{e_shoff:X}, shnum={e_shnum}, shstrndx={e_shstrndx}")
    
    # Procurar seção .symtab
    symtab_offset = None
    strtab_offset = None
    strtab_size = None
    
    for i in range(e_shnum):
        sh_offset = e_shoff + (i * 40)  # 40 bytes por section header (32-bit ELF)
        sh_type = struct.unpack('<I', elf_data[sh_offset+4:sh_offset+8])[0]
        sh_name_off = struct.unpack('<I', elf_data[sh_offset:sh_offset+4])[0]
        
        if sh_type == 2:  # SHT_SYMTAB
            symtab_offset = struct.unpack('<I', elf_data[sh_offset+16:sh_offset+20])[0]
            print(f"[INFO] Seção .symtab encontrada em offset 0x{symtab_offset:X}")
        elif sh_type == 3:  # SHT_STRTAB
            strtab_offset = struct.unpack('<I', elf_data[sh_offset+16:sh_offset+20])[0]
            strtab_size = struct.unpack('<I', elf_data[sh_offset+20:sh_offset+24])[0]
            print(f"[INFO] Seção .strtab encontrada em offset 0x{strtab_offset:X}, tamanho {strtab_size}")
    
    if not symtab_offset:
        print("[ERRO] Seção .symtab não encontrada")
        return None
    
    # Procurar símbolo _gp ou __gnu_local_gp
    gp_candidates = ['_gp', '__gnu_local_gp', 'gp']
    found_gp = None
    
    # Ler tabela de símbolos
    sym_size = 16  # Tamanho de cada entrada de símbolo
    while symtab_offset + sym_size <= len(elf_data):
        st_name = struct.unpack('<I', elf_data[symtab_offset:symtab_offset+4])[0]
        st_value = struct.unpack('<I', elf_data[symtab_offset+4:symtab_offset+8])[0]
        st_shndx = struct.unpack('<H', elf_data[symtab_offset+14:symtab_offset+16])[0]
        
        # Extrair nome do símbolo
        if strtab_offset and st_name < strtab_size:
            name_offset = strtab_offset + st_name
            name_end = elf_data.find(b'\x00', name_offset)
            sym_name = elf_data[name_offset:name_end].decode('utf-8', errors='ignore')
            
            if sym_name in gp_candidates:
                print(f"[ENCONTRADO] Símbolo '{sym_name}': value=0x{st_value:08X}, shndx={st_shndx}")
                if found_gp is None or sym_name == '_gp':  # Preferir _gp
                    found_gp = st_value
        
        symtab_offset += sym_size
    
    return found_gp

def update_json_gp(json_path, gp_value):
    """Atualiza o elf_meta.json com o GP correto."""
    try:
        with open(json_path, 'r') as f:
            data = json.load(f)
    except FileNotFoundError:
        print(f"[ERRO] Arquivo {json_path} não encontrado")
        return False
    
    old_gp = data.get('gp_value', None)
    data['gp_value'] = gp_value
    
    with open(json_path, 'w') as f:
        json.dump(data, f, indent=2)
    
    print(f"[ATUALIZADO] {json_path}")
    print(f"  GP anterior: 0x{old_gp:08X} ({old_gp})" if old_gp else "  GP anterior: não definido")
    print(f"  GP novo:     0x{gp_value:08X} ({gp_value})")
    
    return True

if __name__ == '__main__':
    elf_file = 'elf_meta_image.bin'
    json_file = 'elf_meta.json'
    
    print("[EXECUTANDO] Extração de GP do ELF...")
    gp_value = extract_gp_from_elf(elf_file)
    
    if gp_value is not None:
        print(f"\n[SUCESSO] GP encontrado: 0x{gp_value:08X}")
        update_json_gp(json_file, gp_value)
    else:
        print("\n[FALHA] Não foi possível extrair o GP")
        sys.exit(1)
