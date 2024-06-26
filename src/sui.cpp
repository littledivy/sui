#include <LIEF/LIEF.hpp>
#include <algorithm>
#include <codecvt>
#include <locale>
#include <memory>
#include <vector>

#include "sui.h"

enum class InjectResult { kAlreadyExists, kError, kSuccess };

ExecutableFormat get_executable_format(const char* start, size_t length) {
  std::vector<uint8_t> buffer(start, start + length);
  LIEF::logging::disable();

  if (LIEF::ELF::is_elf(buffer)) {
    return ExecutableFormat::kELF;
  } else if (LIEF::MachO::is_macho(buffer)) {
    return ExecutableFormat::kMachO;
  } else if (LIEF::PE::is_pe(buffer)) {
    return ExecutableFormat::kPE;
  }

  return ExecutableFormat::kUnknown;
}

int inject_into_elf(const uint8_t* executable_ptr, size_t executable_size,
                    const char* note_name_ptr, size_t note_name_size,
                    const uint8_t* data_ptr, size_t data_size, bool overwrite,
                    void* user_data, Write write) {
  std::vector<uint8_t> executable(executable_ptr,
                                  executable_ptr + executable_size);
  std::string note_name(note_name_ptr, note_name_ptr + note_name_size);
  std::vector<uint8_t> data(data_ptr, data_ptr + data_size);

  std::unique_ptr<LIEF::ELF::Binary> binary =
      LIEF::ELF::Parser::parse(executable);

  if (!binary) {
    // error
  }

  LIEF::ELF::Note* existing_note = nullptr;

  for (LIEF::ELF::Note& note : binary->notes()) {
    if (note.name() == note_name) {
      existing_note = &note;
    }
  }

  if (existing_note) {
    if (!overwrite) {
      // error
    } else {
      binary->remove(*existing_note);
    }
  }

  LIEF::ELF::Note note;
  note.name(note_name);
  note.description(data);
  binary->add(note);

  std::vector<uint8_t> output = binary->raw();
  return write(output.data(), output.size(), user_data);
}

int inject_into_macho(const uint8_t* executable_ptr, size_t executable_size,
                      const char* segment_name_ptr, size_t segment_name_size,
                      const char* section_name_ptr, size_t section_name_size,
                      const uint8_t* data_ptr, size_t data_size, bool overwrite,
                      void* user_data, Write write) {
  std::vector<uint8_t> executable(executable_ptr,
                                  executable_ptr + executable_size);
  std::string segment_name(segment_name_ptr,
                           segment_name_ptr + segment_name_size);
  std::string section_name(section_name_ptr,
                           section_name_ptr + section_name_size);
  std::vector<uint8_t> data(data_ptr, data_ptr + data_size);

  std::unique_ptr<LIEF::MachO::FatBinary> fat_binary =
      LIEF::MachO::Parser::parse(executable);

  if (!fat_binary) {
    // error
  }

  // Inject into all Mach-O binaries if there's more than one in a fat binary
  for (LIEF::MachO::Binary& binary : *fat_binary) {
    LIEF::MachO::Section* existing_section =
        binary.get_section(segment_name, section_name);

    if (existing_section) {
      if (!overwrite) {
        // error
      }

      binary.remove_section(segment_name, section_name, true);
    }

    LIEF::MachO::SegmentCommand* segment = binary.get_segment(segment_name);
    LIEF::MachO::Section section(section_name, data);

    if (!segment) {
      // Create the segment and mark it read-only
      LIEF::MachO::SegmentCommand new_segment(segment_name);
      new_segment.max_protection(
          static_cast<uint32_t>(LIEF::MachO::VM_PROTECTIONS::VM_PROT_READ));
      new_segment.init_protection(
          static_cast<uint32_t>(LIEF::MachO::VM_PROTECTIONS::VM_PROT_READ));
      new_segment.add_section(section);
      binary.add(new_segment);
    } else {
      binary.add_section(*segment, section);
    }

    // It will need to be signed again anyway, so remove the signature
    if (binary.has_code_signature()) {
      binary.remove_signature();
    }
  }

  std::vector<uint8_t> output = fat_binary->raw();
  return write(output.data(), output.size(), user_data);
}

int inject_into_pe(const uint8_t* executable_ptr, size_t executable_size,
                   const char* resource_name_ptr, size_t resource_name_size,
                   const uint8_t* data_ptr, size_t data_size, bool overwrite,
                   void* user_data, Write write) {
  std::vector<uint8_t> executable(executable_ptr,
                                  executable_ptr + executable_size);
  std::string resource_name(resource_name_ptr,
                            resource_name_ptr + resource_name_size);
  std::vector<uint8_t> data(data_ptr, data_ptr + data_size);

  std::unique_ptr<LIEF::PE::Binary> binary =
      LIEF::PE::Parser::parse(executable);

  if (!binary) {
    std::cout << "Failed to parse binary\n";
    return -1;
  }

  // TODO - lief.PE.ResourcesManager doesn't support RCDATA it seems, add
  // support so this is simpler?

  if (!binary->has_resources()) {
    std::cout << "No resources\n";
    return -1;
  }

  LIEF::PE::ResourceNode* resources = binary->resources();

  LIEF::PE::ResourceNode* rcdata_node = nullptr;
  LIEF::PE::ResourceNode* id_node = nullptr;

  // First level => Type (ResourceDirectory node)
  auto rcdata_node_iter = std::find_if(
      std::begin(resources->childs()), std::end(resources->childs()),
      [](const LIEF::PE::ResourceNode& node) {
        return node.id() ==
               static_cast<uint32_t>(LIEF::PE::RESOURCE_TYPES::RCDATA);
      });

  if (rcdata_node_iter != std::end(resources->childs())) {
    rcdata_node = &*rcdata_node_iter;
  } else {
    LIEF::PE::ResourceDirectory new_rcdata_node;
    new_rcdata_node.id(static_cast<uint32_t>(LIEF::PE::RESOURCE_TYPES::RCDATA));
    rcdata_node = &resources->add_child(new_rcdata_node);
  }

  // Second level => ID (ResourceDirectory node)
  auto id_node_iter = std::find_if(
      std::begin(rcdata_node->childs()), std::end(rcdata_node->childs()),
      [resource_name](const LIEF::PE::ResourceNode& node) {
        return node.name() ==
               std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,
                                    char16_t>{}
                   .from_bytes(resource_name);
      });

  if (id_node_iter != std::end(rcdata_node->childs())) {
    id_node = &*id_node_iter;
  } else {
    LIEF::PE::ResourceDirectory new_id_node;
    new_id_node.name(resource_name);
    // TODO - This isn't documented, but if this isn't set then LIEF won't save
    //        the name. Seems like LIEF should be able to automatically handle
    //        this if you've set the node's name
    new_id_node.id(0x80000000);
    id_node = &rcdata_node->add_child(new_id_node);
  }

  // Third level => Lang (ResourceData node)
  if (id_node->childs() != std::end(id_node->childs())) {
    if (!overwrite) {
      // error
      std::cout << "Resource already exists and overwrite is false\n";
      return -1;
    }

    id_node->delete_child(*id_node->childs());
  }

  LIEF::PE::ResourceData lang_node;
  lang_node.content(data);
  id_node->add_child(lang_node);

  binary->remove_section(".rsrc", true);

  // Write out the binary, only modifying the resources
  LIEF::PE::Builder builder(*binary);
  builder.build_dos_stub(true);
  builder.build_imports(false);
  builder.build_overlay(false);
  builder.build_relocations(false);
  builder.build_resources(true);
  builder.build_tls(false);
  builder.build();

  // TODO - Why doesn't LIEF just replace the .rsrc section?
  //        Can we at least change build_resources to take a section name?

  // Re-parse the output so the .l2 section is available
  binary = LIEF::PE::Parser::parse(builder.get_build());

  // Rename the rebuilt resource section
  LIEF::PE::Section* section = binary->get_section(".l2");
  section->name(".rsrc");

  LIEF::PE::Builder builder2(*binary);
  builder2.build_dos_stub(true);
  builder2.build_imports(false);
  builder2.build_overlay(false);
  builder2.build_relocations(false);
  builder2.build_resources(false);
  builder2.build_tls(false);
  builder2.build();

  std::vector<uint8_t> output = builder2.get_build();
  return write(output.data(), output.size(), user_data);
}

