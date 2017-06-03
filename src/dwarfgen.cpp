/* Windows specific header files */
#ifdef HAVE_STDAFX_H
#include "stdafx.h"
#endif /* HAVE_STDAFX_H */

#ifndef __NT__
#define O_BINARY 0
#endif

#include "gelf.h"
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdlib.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "dwarfexport.h"

strtabdata strtab;
int add_section_header_string(Elf *elf, const char *name) {
  std::size_t sh_index;

  if (elf_getshstrndx(elf, &sh_index) == -1) {
    dwarfexport_error("elf_getshstrndx() failed: ", elf_errmsg(-1));
  }

  std::cout << "Section header table is at " << sh_index << std::endl;

  auto strscn = elf_getscn(elf, sh_index);
  if (strscn == NULL) {
    dwarfexport_error("elf_getscn() failed: ", elf_errmsg(-1));
  }

  GElf_Shdr shdr;
  if (gelf_getshdr(strscn, &shdr) == NULL) {
    dwarfexport_error("gelf_getshdr() failed: ", elf_errmsg(-1));
  }

  Elf_Data *data;
  if ((data = elf_getdata(strscn, NULL)) == NULL) {
    dwarfexport_error("elf_getdata() failed: ", elf_errmsg(-1));
  }
  std::cout << "Got a data: " << (void *)data << std::endl;
  std::cout << "Existing section header table has length " << data->d_size
            << std::endl;

  strtab.loadExistingTable((char *)data->d_buf, data->d_size);
  auto ret = strtab.addString(name);

  data->d_buf = strtab.exposedata();
  data->d_size = strtab.exposelen();
  shdr.sh_size = data->d_size;

  std::cout << "Setting buffer len to " << data->d_size << std::endl;

  if (!gelf_update_shdr(strscn, &shdr)) {
    dwarfexport_error("Unable to gelf_update_shdr()", elf_errmsg(-1));
  }

  return ret;
}

int callback(const char *name, int size, Dwarf_Unsigned type,
             Dwarf_Unsigned flags, Dwarf_Unsigned link, Dwarf_Unsigned info,
             Dwarf_Unsigned *sect_name_symbol_index, void *userdata, int *) {
  DwarfGenInfo &geninfo = *(DwarfGenInfo *)userdata;
  auto elf = geninfo.elf;

  if (strncmp(name, ".rel", 4) == 0) {
    return 0;
  }

  Elf_Scn *scn = elf_newscn(elf);
  if (!scn) {
    dwarfexport_error("Unable to elf_newscn(): ", elf_errmsg(-1));
  }

  GElf_Shdr shdr;
  if (!gelf_getshdr(scn, &shdr)) {
    dwarfexport_error("Unable to elf_getshdr(): ", elf_errmsg(-1));
  }

  shdr.sh_type = type;
  shdr.sh_flags = flags;
  shdr.sh_addr = 0;
  shdr.sh_link = link;
  shdr.sh_info = info;
  shdr.sh_addralign = 1;
  shdr.sh_name = add_section_header_string(elf, name);

  std::cout << "Added section '" << name << "' index #" << elf_ndxscn(scn)
            << " (sh_type=" << type << ")" << std::endl;

  // We set these correctly later
  shdr.sh_size = 0;
  shdr.sh_offset = 0;
  shdr.sh_entsize = 0;

  if (!gelf_update_shdr(scn, &shdr)) {
    dwarfexport_error("Unable to gelf_update_shdr()", elf_errmsg(-1));
  }

  return elf_ndxscn(scn);
}

std::shared_ptr<DwarfGenInfo> generate_dwarf_object() {
  auto info = std::make_shared<DwarfGenInfo>();

  int ptrsizeflagbit = DW_DLC_POINTER32;
  int offsetsizeflagbit = DW_DLC_OFFSET32;
  if (info->mode == Mode::BIT64) {
    ptrsizeflagbit = DW_DLC_POINTER64;
  }

  const char *isa_name = (info->mode == Mode::BIT32) ? "x86" : "x86_64";
  const char *dwarf_version = "V2";

  int endian = (inf.mf) ? DW_DLC_TARGET_BIGENDIAN : DW_DLC_TARGET_LITTLEENDIAN;
  Dwarf_Ptr errarg = 0;
  Dwarf_Error err = 0;

  int res =
      dwarf_producer_init(DW_DLC_WRITE | ptrsizeflagbit | offsetsizeflagbit |
                              DW_DLC_SYMBOLIC_RELOCATIONS | endian,
                          callback, 0, errarg, (void *)info.get(), isa_name,
                          dwarf_version, 0, &info->dbg, &err);
  if (res != DW_DLV_OK) {
    dwarfexport_error("dwarf_producer_init failed");
  }
  res = dwarf_pro_set_default_string_form(info->dbg, DW_FORM_string, &err);
  if (res != DW_DLV_OK) {
    dwarfexport_error("dwarf_pro_set_default_string_form failed");
  }

  return info;
}

static int get_elf_machine_type(Mode m) {
  switch (ph.id) {
  case PLFM_386:
    return (m == Mode::BIT32) ? EM_386 : EM_X86_64;
  case PLFM_PPC:
    return (m == Mode::BIT32) ? EM_PPC : EM_PPC64;
  case PLFM_ARM:
    return (m == Mode::BIT32) ? EM_ARM : EM_AARCH64;
  default:
    msg("Unknown processor type, using EM_386");
    return EM_386;
  }
}

static Elf_Scn *get_last_section(Elf *elf) {
  std::size_t count, max_offset = 0;
  GElf_Shdr shdr;
  Elf_Scn *last_scn;

  if (elf_getshdrnum(elf, &count) == -1) {
    dwarfexport_error("elf_getshdrnum() failed: ", elf_errmsg(-1));
  }
  for (std::size_t i = 0; i < count; ++i) {
    Elf_Scn *scn = elf_getscn(elf, i);
    if (!gelf_getshdr(scn, &shdr)) {
      dwarfexport_error("elf_getshdr() failed: ", elf_errmsg(-1));
    }
    if (shdr.sh_offset > max_offset) {
      last_scn = scn;
      max_offset = shdr.sh_offset;
    }
  }
  return last_scn;
}

static off_t get_current_data_offset(Elf_Scn *scn) {
  Elf_Data *data = NULL;
  off_t offset = 0;
  while ((data = elf_getdata(scn, data)) != NULL) {
    if (data->d_off >= offset) {
      offset = data->d_off + data->d_size;
    }

    // This shouldn't be necessary, but libelf complains the
    // version is unknown otherwise.
    data->d_version = EV_CURRENT;
  }
  return offset;
}

static void add_data_to_section_end(Elf *elf, Elf_Scn *scn, void *bytes,
                                    std::size_t length, Elf_Type type) {

  std::cout << "Adding data with length = " << length << std::endl;

  auto data_offset = get_current_data_offset(scn);
  Elf_Data *ed = elf_newdata(scn);
  if (!ed) {
    dwarfexport_error("elf_newdata() failed: ", elf_errmsg(-1));
  }
  ed->d_buf = bytes;
  ed->d_type = type;
  ed->d_size = length;
  ed->d_align = 1;
  ed->d_version = EV_CURRENT;
  ed->d_off = data_offset;

  std::cout << "Setting data offset to " << ed->d_off << std::endl;

  // Update the section size and offset
  Elf_Scn *last_scn = get_last_section(elf);
  GElf_Shdr shdr, last_shdr;
  if (!gelf_getshdr(scn, &shdr) || !gelf_getshdr(last_scn, &last_shdr)) {
    dwarfexport_error("elf_getshdr() failed: ", elf_errmsg(-1));
  }

  if (!shdr.sh_offset) {
    shdr.sh_offset = last_shdr.sh_offset + last_shdr.sh_size;
    std::cout << "New section offset = " << shdr.sh_offset << " ("
              << last_shdr.sh_offset << ", " << last_shdr.sh_size << ")"
              << std::endl;
  }
  shdr.sh_size += length;
  std::cout << "New section size = " << shdr.sh_size << std::endl;
  if (!gelf_update_shdr(scn, &shdr)) {
    dwarfexport_error("gelf_update_shdr() failed: ", elf_errmsg(-1));
  }
}

static void add_debug_section_data(std::shared_ptr<DwarfGenInfo> info) {
  auto dbg = info->dbg;
  auto elf = info->elf;

  // Invokes the callback to create the needed sections
  Dwarf_Signed sectioncount = dwarf_transform_to_disk_form(dbg, 0);

  for (Dwarf_Signed d = 0; d < sectioncount; ++d) {
    Dwarf_Signed elf_section_index = 0;
    Dwarf_Unsigned length = 0;
    Dwarf_Ptr bytes =
        dwarf_get_section_bytes(dbg, d, &elf_section_index, &length, 0);

    std::cout << "Add data for dwarf #" << d << " section #"
              << elf_section_index << std::endl;

    Elf_Scn *scn = elf_getscn(elf, elf_section_index);
    if (!scn) {
      dwarfexport_error("Unable to elf_getscn on disk transform");
    }

    add_data_to_section_end(elf, scn, bytes, length, ELF_T_BYTE);
  }
}

static void generate_copy_with_dbg_info(std::shared_ptr<DwarfGenInfo> info,
                                        const std::string &src,
                                        const std::string &dst) {
  int fd_in = -1, fd_out = -1;
  Elf *elf_in = 0, *elf_out = 0;

  Elf_Scn *scn_in, *scn_out;
  GElf_Ehdr ehdr_in, ehdr_out;

  int scn_index = 0;

  if (elf_version(EV_CURRENT) == EV_NONE)
    dwarfexport_error("ELF library initialization failed: ", elf_errmsg(-1));

  if ((fd_in = open(src.c_str(), O_RDONLY, 0)) < 0)
    dwarfexport_error("open failed: ", src);

  if ((elf_in = elf_begin(fd_in, ELF_C_READ, NULL)) == NULL)
    dwarfexport_error("elf_begin() failed: ", elf_errmsg(-1));

  if (gelf_getehdr(elf_in, &ehdr_in) != &ehdr_in)
    dwarfexport_error("gelf_getehdr() failed: ", elf_errmsg(-1));

  /* Checks and warns */
  if (elf_kind(elf_in) != ELF_K_ELF) {
    dwarfexport_error(src, " : ", dst, " must be an ELF file.");
  }

  /* open output elf */
  if ((fd_out = open(dst.c_str(), O_WRONLY | O_CREAT, 0777)) < 0)
    dwarfexport_error("open failed: ", dst);

  if ((elf_out = elf_begin(fd_out, ELF_C_WRITE, NULL)) == NULL)
    dwarfexport_error("elf_begin() failed: ", elf_errmsg(-1));

  /* create new elf header */
  if (gelf_newehdr(elf_out, ehdr_in.e_ident[EI_CLASS]) == 0)
    dwarfexport_error("gelf_newehdr() failed: ", elf_errmsg(-1));

  info->elf = elf_out;

  /* Some compilers produce binaries with non-adjacent sections, so
   * we cannot use the automatic layout. Suppress it and use the exact
   * layout from the input. */
  if (elf_flagelf(elf_out, ELF_C_SET, ELF_F_LAYOUT) == 0)
    dwarfexport_error("elf_flagelf failed: ", elf_errmsg(-1));

  if (gelf_getehdr(elf_out, &ehdr_out) != &ehdr_out)
    dwarfexport_error("gelf_getehdr() failed: ", elf_errmsg(-1));

  ehdr_out = ehdr_in;

  if (gelf_update_ehdr(elf_out, &ehdr_out) == 0)
    dwarfexport_error("gelf_update_ehdr() failed: ", elf_errmsg(-1));

  GElf_Phdr phdr_in, phdr_out;
  int ph_ndx;

  if (ehdr_in.e_phnum && gelf_newphdr(elf_out, ehdr_in.e_phnum) == 0)
    dwarfexport_error("gelf_newphdr() failed: ", elf_errmsg(-1));

  for (ph_ndx = 0; ph_ndx < ehdr_in.e_phnum; ++ph_ndx) {
    if (gelf_getphdr(elf_in, ph_ndx, &phdr_in) != &phdr_in)
      dwarfexport_error("gelf_getphdr() failed: ", elf_errmsg(-1));

    if (gelf_getphdr(elf_out, ph_ndx, &phdr_out) != &phdr_out)
      dwarfexport_error("gelf_getphdr() failed: ", elf_errmsg(-1));

    phdr_out = phdr_in;

    if (gelf_update_phdr(elf_out, ph_ndx, &phdr_out) == 0)
      dwarfexport_error("gelf_update_phdr() failed: ", elf_errmsg(-1));
  }

  /* copy sections to new elf */
  Elf_Data *data_in, *data_out;
  GElf_Shdr shdr_in, shdr_out;
  for (scn_index = 1; scn_index < ehdr_in.e_shnum; scn_index++) {
    std::cout << "Creating section with index = " << scn_index << std::endl;
    if ((scn_in = elf_getscn(elf_in, scn_index)) == NULL)
      dwarfexport_error("getshdr() failed: ", elf_errmsg(-1));
    if ((scn_out = elf_newscn(elf_out)) == NULL)
      dwarfexport_error("elf_newscn() failed: ", elf_errmsg(-1));

    if (gelf_getshdr(scn_in, &shdr_in) != &shdr_in)
      dwarfexport_error("getshdr() failed: ", elf_errmsg(-1));

    data_in = NULL;
    while ((data_in = elf_getdata(scn_in, data_in)) != NULL) {
      std::cout << "Adding data" << std::endl;
      if ((data_out = elf_newdata(scn_out)) == NULL)
        dwarfexport_error("elf_newdata() failed: ", elf_errmsg(-1));

      *data_out = *data_in;
    }

    if (gelf_getshdr(scn_out, &shdr_out) != &shdr_out)
      dwarfexport_error("gelf_getshdr() failed: ", elf_errmsg(-1));

    shdr_out = shdr_in;

    if (gelf_update_shdr(scn_out, &shdr_out) == 0)
      dwarfexport_error("gelf_update_shdr() failed: ", elf_errmsg(-1));
  }

  add_debug_section_data(info);

  // Fix the section header start location
  auto last_scn = get_last_section(elf_out);
  GElf_Shdr last_shdr;
  if (gelf_getshdr(last_scn, &last_shdr) != &last_shdr)
    dwarfexport_error("gelf_getshdr() failed: ", elf_errmsg(-1));

  ehdr_out.e_shoff = last_shdr.sh_offset + last_shdr.sh_size;
  if (gelf_update_ehdr(elf_out, &ehdr_out) == 0)
    dwarfexport_error("gelf_update_ehdr() failed: ", elf_errmsg(-1));

  if (elf_update(elf_out, ELF_C_WRITE) < 0)
    dwarfexport_error("elf_update() failed: ", elf_errmsg(-1));

  elf_end(elf_out);
  close(fd_out);
  elf_end(elf_in);
  close(fd_in);
}

void write_dwarf_file(std::shared_ptr<DwarfGenInfo> info,
                      const Options &options) {
  if (options.attach_debug_info) {
    generate_copy_with_dbg_info(info, options.filename, options.dbg_filename());
  } else {
    // generate_file_with_dbg_info(info, options.dbg_filename());
  }
  dwarf_producer_finish(info->dbg, 0);
}