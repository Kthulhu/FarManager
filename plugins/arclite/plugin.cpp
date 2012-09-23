#include "msg.h"
#include "plugin.h"
#include "guids.hpp"

#include "utils.hpp"
#include "sysutils.hpp"
#include "farutils.hpp"
#include "common.hpp"
#include "ui.hpp"
#include "archive.hpp"
#include "options.hpp"
#include "cmdline.hpp"

wstring g_plugin_prefix;
TriState g_detect_next_time = triUndef;

void attach_sfx_module(const wstring& file_path, const SfxOptions& sfx_options);

class Plugin {
private:
  shared_ptr<Archive> archive;

  wstring current_dir;
  wstring extract_dir;
  wstring created_dir;
  wstring host_file;
  wstring panel_title;
  vector<InfoPanelLine> info_lines;

public:
  Plugin(): archive(new Archive()) {
  }

  static Plugin* open(const Archives& archives) {
    if (archives.size() == 0)
      FAIL(E_ABORT);

    int format_idx;
    if (archives.size() == 1) {
      format_idx = 0;
    }
    else {
      Far::MenuItems format_names;
      for (unsigned i = 0; i < archives.size(); i++) {
        format_names.add(archives[i]->arc_chain.to_string());
      }
      format_idx = Far::menu(c_format_menu_guid, Far::get_msg(MSG_PLUGIN_NAME), format_names);
      if (format_idx == -1)
        FAIL(E_ABORT);
    }

    unique_ptr<Plugin> plugin(new Plugin());
    plugin->archive = archives[format_idx];
    return plugin.release();
  }

  void info(OpenPanelInfo* opi) {
    opi->StructSize = sizeof(OpenPanelInfo);
    opi->Flags = OPIF_ADDDOTS | OPIF_SHORTCUT;
    opi->CurDir = current_dir.c_str();
    panel_title = Far::get_msg(MSG_PLUGIN_NAME);
    if (archive->is_open()) {
      panel_title += L":" + archive->arc_chain.to_string() + L":" + archive->arc_name();
      if (!current_dir.empty())
        panel_title += L":" + current_dir;
      host_file = archive->arc_path;
      if (archive->has_crc)
        opi->Flags |= OPIF_USECRC32;
    }
    opi->HostFile = host_file.c_str();
    opi->Format = g_plugin_prefix.c_str();
    opi->PanelTitle = panel_title.c_str();
    if (g_options.own_panel_view_mode) {
      opi->StartPanelMode = '0' + g_options.panel_view_mode;
      opi->StartSortMode = g_options.panel_sort_mode;
      opi->StartSortOrder = g_options.panel_reverse_sort;
    }

    info_lines.clear();
    info_lines.reserve(archive->arc_attr.size() + 1);
    InfoPanelLine ipl;
    for_each(archive->arc_attr.begin(), archive->arc_attr.end(), [&] (const Attr& attr) {
      ipl.Text = attr.name.c_str();
      ipl.Data = attr.value.c_str();
      ipl.Flags = 0;
      info_lines.push_back(ipl);
    });
    opi->InfoLines = info_lines.data();
    opi->InfoLinesNumber = static_cast<int>(info_lines.size());
  }

  void set_dir(const wstring& dir) {
    if (!archive->is_open())
      FAIL(E_ABORT);
    wstring new_dir;
    if (dir == L"\\")
      new_dir.assign(dir);
    else if (dir == L"..")
      new_dir = extract_file_path(current_dir);
    else if (dir[0] == L'\\') // absolute path
      new_dir.assign(dir);
    else
      new_dir.assign(L"\\").append(add_trailing_slash(remove_path_root(current_dir))).append(dir);

    if (new_dir == L"\\")
      new_dir.clear();

    archive->find_dir(new_dir);
    current_dir = new_dir;
  }

  void list(PluginPanelItem** panel_items, size_t* items_number) {
    if (!archive->is_open())
      FAIL(E_ABORT);
    UInt32 dir_index = archive->find_dir(current_dir);
    FileIndexRange dir_list = archive->get_dir_list(dir_index);
    size_t size = dir_list.second - dir_list.first;
    unique_ptr<PluginPanelItem[]> items(new PluginPanelItem[size]);
    memset(items.get(), 0, size * sizeof(PluginPanelItem));
    unsigned idx = 0;
    for_each(dir_list.first, dir_list.second, [&] (UInt32 file_index) {
      const ArcFileInfo& file_info = archive->file_list[file_index];
      const DWORD c_valid_attributes = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM;
      items[idx].FileAttributes = archive->get_attr(file_index) & c_valid_attributes;
      items[idx].CreationTime = archive->get_ctime(file_index);
      items[idx].LastAccessTime = archive->get_atime(file_index);
      items[idx].LastWriteTime = archive->get_mtime(file_index);
      items[idx].FileSize = archive->get_size(file_index);
      items[idx].AllocationSize = archive->get_psize(file_index);
      items[idx].FileName = const_cast<wchar_t*>(file_info.name.c_str());
      items[idx].UserData.UserData = reinterpret_cast<void*>(file_index);
      items[idx].CRC32 = archive->get_crc(file_index);
      idx++;
    });
    *panel_items = items.release();
    *items_number = size;
  }

  static wstring get_separate_dir_path(const wstring& dst_dir, const wstring& arc_name) {
    wstring final_dir = add_trailing_slash(dst_dir) + arc_name;
    wstring ext = extract_file_ext(final_dir);
    final_dir.erase(final_dir.size() - ext.size(), ext.size());
    final_dir = auto_rename(final_dir);
    return final_dir;
  }

  class PluginPanelItemAccessor {
  public:
    virtual const PluginPanelItem* get(size_t idx) const = 0;
    virtual size_t size() const = 0;
  };

  void extract(const PluginPanelItemAccessor& panel_items, wstring& dst_dir, bool move, OPERATION_MODES op_mode) {
    if (panel_items.size() == 0)
      return;
    bool single_item = panel_items.size() == 1;
    if (single_item && wcscmp(panel_items.get(0)->FileName, L"..") == 0)
      return;
    ExtractOptions options;
    options.dst_dir = dst_dir;
    options.move_files = archive->updatable() ? (move ? triTrue : triFalse) : triUndef;
    options.delete_archive = false;
    bool show_dialog = (op_mode & (OPM_FIND | OPM_VIEW | OPM_EDIT | OPM_QUICKVIEW)) == 0;
    if (show_dialog && (op_mode & OPM_SILENT) && (op_mode & OPM_TOPLEVEL) == 0)
      show_dialog = false;
    if (op_mode & (OPM_FIND | OPM_QUICKVIEW))
      options.ignore_errors = true;
    else
      options.ignore_errors = g_options.extract_ignore_errors;
    if (show_dialog) {
      options.overwrite = g_options.extract_overwrite;
      options.separate_dir = g_options.extract_separate_dir;
      options.open_dir = (op_mode & OPM_TOPLEVEL) == 0 ? triUndef : (g_options.extract_open_dir ? triTrue : triFalse);
    }
    else {
      options.overwrite = oaOverwrite;
      options.separate_dir = triFalse;
    }
    if (show_dialog) {
      if (!extract_dialog(options))
        FAIL(E_ABORT);
      if (options.dst_dir.empty())
        options.dst_dir = L".";
      if (!is_absolute_path(options.dst_dir))
        options.dst_dir = Far::get_absolute_path(options.dst_dir);
      dst_dir = options.dst_dir;
      if (options.separate_dir == triTrue || (options.separate_dir == triUndef && !single_item && (op_mode & OPM_TOPLEVEL))) {
        options.dst_dir = get_separate_dir_path(options.dst_dir, archive->arc_name());
      }
      if (!options.password.empty())
        archive->password = options.password;
    }

    UInt32 src_dir_index = archive->find_dir(current_dir);

    vector<UInt32> indices;
    indices.reserve(panel_items.size());
    for (size_t i = 0; i < panel_items.size(); i++) {
      indices.push_back(reinterpret_cast<UInt32>(panel_items.get(i)->UserData.UserData));
    }

    shared_ptr<ErrorLog> error_log(new ErrorLog());
    vector<UInt32> extracted_indices;
    archive->extract(src_dir_index, indices, options, error_log, options.move_files == triTrue ? &extracted_indices : nullptr);

    if (!error_log->empty() && show_dialog) {
      show_error_log(*error_log);
    }

    if (error_log->empty()) {
      if (options.delete_archive) {
        archive->close();
        archive->delete_archive();
        Far::close_panel(this, archive->arc_dir());
      }
      else if (options.move_files == triTrue) {
        archive->delete_files(extracted_indices);
      }
      Far::progress_notify();
    }

    if (options.open_dir == triTrue) {
      if (single_item)
        Far::panel_go_to_file(PANEL_ACTIVE, add_trailing_slash(options.dst_dir) + panel_items.get(0)->FileName);
      else
        Far::panel_go_to_dir(PANEL_ACTIVE, options.dst_dir);
    }
  }

  void get_files(const PluginPanelItem* panel_items, int items_number, int move, const wchar_t** dest_path, OPERATION_MODES op_mode) {
    class PluginPanelItems: public PluginPanelItemAccessor {
    private:
      const PluginPanelItem* panel_items;
      size_t items_number;
    public:
      PluginPanelItems(const PluginPanelItem* panel_items, size_t items_number): panel_items(panel_items), items_number(items_number) {
      }
      virtual const PluginPanelItem* get(size_t idx) const {
        return panel_items + idx;
      }
      virtual size_t size() const {
        return items_number;
      }
    };
    PluginPanelItems pp_items(panel_items, items_number);
    extract_dir = *dest_path;
    extract(pp_items, extract_dir, move != 0, op_mode);
    if (extract_dir != *dest_path) {
      *dest_path = extract_dir.c_str();
    }
  }

  void extract() {
    class PluginPanelItems: public PluginPanelItemAccessor {
    private:
      HANDLE h_plugin;
      PanelInfo panel_info;
      mutable Buffer<unsigned char> buf;
    public:
      PluginPanelItems(HANDLE h_plugin): h_plugin(h_plugin) {
        CHECK(Far::get_panel_info(h_plugin, panel_info));
      }
      virtual const PluginPanelItem* get(size_t idx) const {
        Far::get_panel_item(h_plugin, FCTL_GETSELECTEDPANELITEM, idx, buf);
        return reinterpret_cast<const PluginPanelItem*>(buf.data());
      }
      virtual size_t size() const {
        return panel_info.SelectedItemsNumber;
      }
    };
    PluginPanelItems pp_items(this);
    extract(pp_items, extract_file_path(archive->arc_path), false, 0);
  }

  static void extract(const vector<wstring>& arc_list, ExtractOptions options) {
    wstring dst_dir = options.dst_dir;
    wstring dst_file_name;
    shared_ptr<ErrorLog> error_log(new ErrorLog());
    for (unsigned i = 0; i < arc_list.size(); i++) {
      unique_ptr<Archives> archives;
      try {
        OpenOptions open_options;
        open_options.arc_path = arc_list[i];
        open_options.detect = false;
        open_options.password = options.password;
        open_options.arc_types = ArcAPI::formats().get_arc_types();
        archives = Archive::open(open_options);
        if (archives->empty())
          throw Error(Far::get_msg(MSG_ERROR_NOT_ARCHIVE), arc_list[i], __FILE__, __LINE__);
      }
      catch (const Error& error) {
        if (error.code == E_ABORT)
          throw;
        error_log->push_back(error);
        continue;
      }

      shared_ptr<Archive> archive = (*archives)[0];
      if (archive->password.empty())
        archive->password = options.password;
      archive->make_index();

      FileIndexRange dir_list = archive->get_dir_list(c_root_index);

      unsigned num_items = dir_list.second - dir_list.first;
      if (arc_list.size() == 1 && num_items == 1) {
        dst_file_name = archive->file_list[*dir_list.first].name;
      }

      vector<UInt32> indices;
      indices.reserve(num_items);
      for_each(dir_list.first, dir_list.second, [&] (UInt32 file_index) {
        indices.push_back(file_index);
      });

      if (options.separate_dir == triTrue || (options.separate_dir == triUndef && indices.size() > 1))
        options.dst_dir = get_separate_dir_path(dst_dir, archive->arc_name());
      else
        options.dst_dir = dst_dir;

      size_t error_count = error_log->size();
      archive->extract(c_root_index, indices, options, error_log);

      if (options.delete_archive && error_count == error_log->size()) {
        archive->close();
        archive->delete_archive();
      }
    }

    if (!error_log->empty()) {
      show_error_log(*error_log);
    }
    else {
      Far::update_panel(PANEL_ACTIVE, false);
      Far::progress_notify();
    }

    if (options.open_dir == triTrue) {
      if (arc_list.size() > 1)
        Far::panel_go_to_dir(PANEL_ACTIVE, dst_dir);
      else if (dst_file_name.empty())
        Far::panel_go_to_dir(PANEL_ACTIVE, options.dst_dir);
      else
        Far::panel_go_to_file(PANEL_ACTIVE, add_trailing_slash(options.dst_dir) + dst_file_name);
    }
  }

  static void bulk_extract(const vector<wstring>& arc_list) {
    ExtractOptions options;
    PanelInfo panel_info;
    if (Far::get_panel_info(PANEL_PASSIVE, panel_info) && Far::is_real_file_panel(panel_info))
      options.dst_dir = Far::get_panel_dir(PANEL_PASSIVE);
    options.move_files = triUndef;
    options.delete_archive = false;
    options.ignore_errors = g_options.extract_ignore_errors;
    options.overwrite = g_options.extract_overwrite;
    options.separate_dir = g_options.extract_separate_dir;
    options.open_dir = g_options.extract_open_dir ? triTrue : triFalse;

    if (!extract_dialog(options))
      FAIL(E_ABORT);
    if (options.dst_dir.empty())
      options.dst_dir = L".";
    if (!is_absolute_path(options.dst_dir))
      options.dst_dir = Far::get_absolute_path(options.dst_dir);

    extract(arc_list, options);
  }

  static void cmdline_extract(const ExtractCommand& cmd) {
    vector<wstring> arc_list;
    arc_list.reserve(cmd.arc_list.size());
    for_each(cmd.arc_list.begin(), cmd.arc_list.end(), [&] (const wstring& arc_name) {
      arc_list.push_back(Far::get_absolute_path(arc_name));
    });
    ExtractOptions options = cmd.options;
    options.dst_dir = Far::get_absolute_path(cmd.options.dst_dir);
    extract(arc_list, options);
  }

  void test_files(struct PluginPanelItem* panel_items, int items_number, OPERATION_MODES op_mode) {
    UInt32 src_dir_index = archive->find_dir(current_dir);
    vector<UInt32> indices;
    indices.reserve(items_number);
    for (int i = 0; i < items_number; i++) {
      indices.push_back(reinterpret_cast<UInt32>(panel_items[i].UserData.UserData));
    }
    archive->test(src_dir_index, indices);
    Far::info_dlg(c_test_ok_dialog_guid, Far::get_msg(MSG_PLUGIN_NAME), Far::get_msg(MSG_TEST_OK));
  }

  static void bulk_test(const vector<wstring>& arc_list) {
    shared_ptr<ErrorLog> error_log(new ErrorLog());
    for (unsigned i = 0; i < arc_list.size(); i++) {
      unique_ptr<Archives> archives;
      try {
        OpenOptions open_options;
        open_options.arc_path = arc_list[i];
        open_options.detect = false;
        open_options.arc_types = ArcAPI::formats().get_arc_types();
        archives = Archive::open(open_options);
        if (archives->empty())
          throw Error(Far::get_msg(MSG_ERROR_NOT_ARCHIVE), arc_list[i], __FILE__, __LINE__);
      }
      catch (const Error& error) {
        if (error.code == E_ABORT)
          throw;
        error_log->push_back(error);
        continue;
      }

      shared_ptr<Archive> archive = (*archives)[0];
      archive->make_index();

      FileIndexRange dir_list = archive->get_dir_list(c_root_index);
      vector<UInt32> indices;
      indices.reserve(dir_list.second - dir_list.first);
      for_each(dir_list.first, dir_list.second, [&] (UInt32 file_index) {
        indices.push_back(file_index);
      });

      try {
        archive->test(c_root_index, indices);
      }
      catch (const Error& error) {
        if (error.code == E_ABORT)
          throw;
        error_log->push_back(error);
      }
    }

    if (!error_log->empty()) {
      show_error_log(*error_log);
    }
    else {
      Far::update_panel(PANEL_ACTIVE, false);
      Far::info_dlg(c_test_ok_dialog_guid, Far::get_msg(MSG_PLUGIN_NAME), Far::get_msg(MSG_TEST_OK));
    }
  }

  static void cmdline_test(const TestCommand& cmd) {
    vector<wstring> arc_list;
    arc_list.reserve(cmd.arc_list.size());
    for_each(cmd.arc_list.begin(), cmd.arc_list.end(), [&] (const wstring& arc_name) {
      arc_list.push_back(Far::get_absolute_path(arc_name));
    });
    Plugin::bulk_test(arc_list);
  }

  void put_files(const PluginPanelItem* panel_items, int items_number, int move, const wchar_t* src_path, OPERATION_MODES op_mode) {
    if (items_number == 1 && wcscmp(panel_items[0].FileName, L"..") == 0)
      return;
    UpdateOptions options;
    bool new_arc = !archive->is_open();
    if (!new_arc && !archive->updatable()) {
      FAIL_MSG(Far::get_msg(MSG_ERROR_NOT_UPDATABLE));
    }
    if (new_arc) {
      if (items_number == 1 || is_root_path(src_path))
        options.arc_path = panel_items[0].FileName;
      else
        options.arc_path = extract_file_name(src_path);
      ArcTypes arc_types = ArcAPI::formats().find_by_name(g_options.update_arc_format_name);
      if (arc_types.empty())
        options.arc_type = c_7z;
      else
        options.arc_type = arc_types.front();
      options.sfx_options = g_options.update_sfx_options;
      options.level = g_options.update_level;
      options.method = g_options.update_method;
      options.solid = g_options.update_solid;
      options.encrypt = false;
      options.encrypt_header = g_options.update_encrypt_header;
      options.volume_size = g_options.update_volume_size;
      options.append_ext = g_options.update_append_ext;
    }
    else {
      options.arc_type = archive->arc_chain.back().type; // required to set update properties
      archive->load_update_props();
      options.level = archive->level;
      options.method = archive->method;
      options.solid = archive->solid;
      options.encrypt = archive->encrypted;
      options.encrypt_header = triUndef;
      options.password = archive->password;
      options.overwrite = g_options.update_overwrite;
      if (op_mode & OPM_EDIT)
        options.overwrite = oaOverwrite;
    }
    options.create_sfx = false;
    options.enable_volumes = false;
    options.show_password = g_options.update_show_password;
    options.move_files = move != 0;
    CHECK(get_app_option(FSSF_SYSTEM, c_copy_opened_files_option, options.open_shared));

    options.ignore_errors = g_options.update_ignore_errors;

    bool res = update_dialog(new_arc, options, g_profiles);
    if (!res)
      FAIL(E_ABORT);
    if (ArcAPI::formats().count(options.arc_type) == 0)
      FAIL_MSG(Far::get_msg(MSG_ERROR_NO_FORMAT));
    if (new_arc) {
      if (File::exists(options.arc_path)) {
        if (Far::message(c_overwrite_archive_dialog_guid, Far::get_msg(MSG_PLUGIN_NAME) + L"\n" + Far::get_msg(MSG_UPDATE_DLG_CONFIRM_OVERWRITE), 0, FMSG_MB_YESNO) != 0)
          FAIL(E_ABORT);
      }
    }
    else {
      archive->level = options.level;
      archive->method = options.method;
      archive->solid = options.solid;
      archive->encrypted = options.encrypt;
    }

    vector<wstring> file_names;
    file_names.reserve(items_number);
    for (int i = 0; i < items_number; i++) {
      file_names.push_back(panel_items[i].FileName);
    }

    shared_ptr<ErrorLog> error_log(new ErrorLog());
    if (new_arc)
      archive->create(src_path, file_names, options, error_log);
    else
      archive->update(src_path, file_names, remove_path_root(current_dir), options, error_log);

    if (!error_log->empty()) {
      show_error_log(*error_log);
    }
    else {
      Far::progress_notify();
    }

    if (new_arc) {
      if (upcase(Far::get_panel_dir(PANEL_ACTIVE)) == upcase(extract_file_path(options.arc_path)))
        Far::panel_go_to_file(PANEL_ACTIVE, options.arc_path);
      if (upcase(Far::get_panel_dir(PANEL_PASSIVE)) == upcase(extract_file_path(options.arc_path)))
        Far::panel_go_to_file(PANEL_PASSIVE, options.arc_path);
    }
  }

  static void create_archive() {
    PanelInfo panel_info;
    if (!Far::get_panel_info(PANEL_ACTIVE, panel_info))
      FAIL(E_ABORT);
    if (!Far::is_real_file_panel(panel_info))
      FAIL(E_ABORT);
    vector<wstring> file_list;
    file_list.reserve(panel_info.SelectedItemsNumber);
    wstring src_path = Far::get_panel_dir(PANEL_ACTIVE);
    for (size_t i = 0; i < panel_info.SelectedItemsNumber; i++) {
      Far::PanelItem panel_item = Far::get_selected_panel_item(PANEL_ACTIVE, i);
      file_list.push_back(panel_item.file_name);
    }
    if (file_list.empty())
      FAIL(E_ABORT);
    if (file_list.size() == 1 && file_list[0] == L"..")
      FAIL(E_ABORT);

    UpdateOptions options;

    if (file_list.size() == 1 || is_root_path(src_path))
      options.arc_path = file_list[0];
    else
      options.arc_path = extract_file_name(src_path);
    ArcTypes arc_types = ArcAPI::formats().find_by_name(g_options.update_arc_format_name);
    if (arc_types.empty())
      options.arc_type = c_7z;
    else
      options.arc_type = arc_types.front();
    options.sfx_options = g_options.update_sfx_options;
    options.level = g_options.update_level;
    options.method = g_options.update_method;
    options.solid = g_options.update_solid;
    options.show_password = g_options.update_show_password;
    options.encrypt = false;
    options.encrypt_header = g_options.update_encrypt_header;
    options.create_sfx = false;
    options.enable_volumes = false;
    options.volume_size = g_options.update_volume_size;
    options.move_files = false;
    CHECK(get_app_option(FSSF_SYSTEM, c_copy_opened_files_option, options.open_shared));
    options.ignore_errors = g_options.update_ignore_errors;
    options.append_ext = g_options.update_append_ext;

    bool res = update_dialog(true, options, g_profiles);
    if (!res)
      FAIL(E_ABORT);
    if (ArcAPI::formats().count(options.arc_type) == 0)
      FAIL_MSG(Far::get_msg(MSG_ERROR_NO_FORMAT));

    if (File::exists(options.arc_path)) {
      if (Far::message(c_overwrite_archive_dialog_guid, Far::get_msg(MSG_PLUGIN_NAME) + L"\n" + Far::get_msg(MSG_UPDATE_DLG_CONFIRM_OVERWRITE), 0, FMSG_MB_YESNO) != 0)
        FAIL(E_ABORT);
    }

    shared_ptr<ErrorLog> error_log(new ErrorLog());
    shared_ptr<Archive>(new Archive())->create(src_path, file_list, options, error_log);

    if (!error_log->empty()) {
      show_error_log(*error_log);
    }
    else {
      Far::progress_notify();
    }

    if (upcase(Far::get_panel_dir(PANEL_ACTIVE)) == upcase(extract_file_path(options.arc_path)))
      Far::panel_go_to_file(PANEL_ACTIVE, options.arc_path);
    if (upcase(Far::get_panel_dir(PANEL_PASSIVE)) == upcase(extract_file_path(options.arc_path)))
      Far::panel_go_to_file(PANEL_PASSIVE, options.arc_path);
  }

  static void cmdline_update(const UpdateCommand& cmd) {
    UpdateOptions options = cmd.options;
    options.arc_path = Far::get_absolute_path(options.arc_path);

    vector<wstring> files;
    files.reserve(cmd.files.size());
    for_each(cmd.files.begin(), cmd.files.end(), [&] (const wstring& file) {
      files.push_back(Far::get_absolute_path(file));
    });

    // load listfiles
    for_each(cmd.listfiles.begin(), cmd.listfiles.end(), [&] (const wstring& listfile) {
      wstring str = load_file(Far::get_absolute_path(listfile));
      std::list<wstring> fl = parse_listfile(str);
      files.reserve(files.size() + fl.size());
      for_each(fl.begin(), fl.end(), [&] (const wstring& file) {
        files.push_back(Far::get_absolute_path(file));
      });
    });

    if (files.empty())
      FAIL(E_ABORT);

    // common source directory
    wstring src_path = extract_file_path(Far::get_absolute_path(files.front()));
    wstring src_path_upcase = upcase(src_path);
    wstring full_path;
    for_each(files.begin(), files.end(), [&] (const wstring& file) {
      while (!substr_match(upcase(file), 0, src_path_upcase.c_str())) {
        if (is_root_path(src_path))
          src_path.clear();
        else
          src_path = extract_file_path(src_path);
        src_path_upcase = upcase(src_path);
      }
    });

    // relative paths
    if (!src_path.empty()) {
      for_each(files.begin(), files.end(), [&] (wstring& file) {
        file.erase(0, src_path.size() + 1);
      });
    }

    CHECK(get_app_option(FSSF_SYSTEM, c_copy_opened_files_option, options.open_shared));

    shared_ptr<ErrorLog> error_log(new ErrorLog());
    if (cmd.new_arc) {
      shared_ptr<Archive>(new Archive())->create(src_path, files, options, error_log);

      if (upcase(Far::get_panel_dir(PANEL_ACTIVE)) == upcase(extract_file_path(options.arc_path)))
        Far::panel_go_to_file(PANEL_ACTIVE, options.arc_path);
      if (upcase(Far::get_panel_dir(PANEL_PASSIVE)) == upcase(extract_file_path(options.arc_path)))
        Far::panel_go_to_file(PANEL_PASSIVE, options.arc_path);
    }
    else {
      OpenOptions open_options;
      open_options.arc_path = options.arc_path;
      open_options.detect = false;
      open_options.password = options.password;
      open_options.arc_types = ArcAPI::formats().get_arc_types();
      unique_ptr<Archives> archives(Archive::open(open_options));
      if (archives->empty())
        throw Error(Far::get_msg(MSG_ERROR_NOT_ARCHIVE), options.arc_path, __FILE__, __LINE__);

      shared_ptr<Archive> archive = (*archives)[0];
      if (!archive->updatable())
        throw Error(Far::get_msg(MSG_ERROR_NOT_UPDATABLE), options.arc_path, __FILE__, __LINE__);

      archive->make_index();

      options.arc_type = archive->arc_chain.back().type;
      archive->load_update_props();
      if (!cmd.level_defined)
        options.level = archive->level;
      if (!cmd.method_defined)
        options.method = archive->method;
      if (!cmd.solid_defined)
        options.solid = archive->solid;
      if (!cmd.encrypt_defined) {
        options.encrypt = archive->encrypted;
        options.password = archive->password;
      }

      archive->update(src_path, files, wstring(), options, error_log);
    }
  }

  void delete_files(const PluginPanelItem* panel_items, int items_number, OPERATION_MODES op_mode) {
    if (items_number == 1 && wcscmp(panel_items[0].FileName, L"..") == 0) return;

    if (!archive->updatable()) {
      FAIL_MSG(Far::get_msg(MSG_ERROR_NOT_UPDATABLE));
    }

    bool show_dialog = (op_mode & (OPM_SILENT | OPM_FIND | OPM_VIEW | OPM_EDIT | OPM_QUICKVIEW)) == 0;
    if (show_dialog) {
      if (Far::message(c_delete_files_dialog_guid, Far::get_msg(MSG_PLUGIN_NAME) + L"\n" + Far::get_msg(MSG_DELETE_DLG_CONFIRM), 0, FMSG_MB_OKCANCEL) != 0)
        FAIL(E_ABORT);
    }

    vector<UInt32> indices;
    indices.reserve(items_number);
    for (int i = 0; i < items_number; i++) {
      indices.push_back(reinterpret_cast<UInt32>(panel_items[i].UserData.UserData));
    }
    archive->delete_files(indices);

    Far::progress_notify();
  }

  void create_dir(const wchar_t** name, OPERATION_MODES op_mode) {
    if (!archive->updatable()) {
      FAIL_MSG(Far::get_msg(MSG_ERROR_NOT_UPDATABLE));
    }
    bool show_dialog = (op_mode & (OPM_SILENT | OPM_FIND | OPM_VIEW | OPM_EDIT | OPM_QUICKVIEW)) == 0;
    created_dir = *name;
    if (show_dialog) {
      if (!Far::input_dlg(c_create_dir_dialog_guid, Far::get_msg(MSG_PLUGIN_NAME), Far::get_msg(MSG_CREATE_DIR_DLG_NAME), created_dir))
        FAIL(E_ABORT);
      *name = created_dir.c_str();
    }
    archive->create_dir(created_dir, remove_path_root(current_dir));
  }

  void show_attr() {
    AttrList attr_list;
    Far::PanelItem panel_item = Far::get_current_panel_item(PANEL_ACTIVE);
    if (panel_item.file_name == L"..") {
      if (is_root_path(current_dir)) {
        attr_list = archive->arc_attr;
      }
      else {
        attr_list = archive->get_attr_list(archive->find_dir(current_dir));
      }
    }
    else {
      attr_list = archive->get_attr_list(reinterpret_cast<UInt32>(panel_item.user_data));
    }
    if (!attr_list.empty())
      attr_dialog(attr_list);
  }

  void close() {
    PanelInfo panel_info;
    if (Far::get_panel_info(this, panel_info)) {
      unsigned panel_view_mode = panel_info.ViewMode;
      OPENPANELINFO_SORTMODES panel_sort_mode = panel_info.SortMode;
      bool panel_reverse_sort = (panel_info.Flags & PFLAGS_REVERSESORTORDER) != 0;
      if (g_options.panel_view_mode != panel_view_mode || g_options.panel_sort_mode != panel_sort_mode || g_options.panel_reverse_sort != panel_reverse_sort) {
        g_options.panel_view_mode = panel_view_mode;
        g_options.panel_sort_mode = panel_sort_mode;
        g_options.panel_reverse_sort = panel_reverse_sort;
        g_options.save();
      }
    }
  }

  static void convert_to_sfx(const vector<wstring>& file_list) {
    SfxOptions sfx_options = g_options.update_sfx_options;
    if (!sfx_options_dialog(sfx_options, g_profiles))
      FAIL(E_ABORT);
    for (unsigned i = 0; i < file_list.size(); i++) {
      attach_sfx_module(file_list[i], sfx_options);
    }
  }
};


void WINAPI GetGlobalInfoW(GlobalInfo* info) {
  info->StructSize = sizeof(GlobalInfo);
  info->MinFarVersion = FARMANAGERVERSION;
  info->Version = PLUGIN_VERSION;
  info->Guid = c_plugin_guid;
  info->Title = PLUGIN_NAME;
  info->Description = PLUGIN_DESCRIPTION;
  info->Author = FARCOMPANYNAME;
}

void WINAPI SetStartupInfoW(const PluginStartupInfo* info) {
  enable_lfh();
  Far::init(info);
  g_options.load();
  g_profiles.load();
  g_plugin_prefix = g_options.plugin_prefix;
  Archive::max_check_size = g_options.max_check_size;
}

void WINAPI GetPluginInfoW(PluginInfo* info) {
  FAR_ERROR_HANDLER_BEGIN;
  static const wchar_t* plugin_menu[1];
  static const wchar_t* config_menu[1];
  plugin_menu[0] = Far::msg_ptr(MSG_PLUGIN_NAME);
  config_menu[0] = Far::msg_ptr(MSG_PLUGIN_NAME);

  info->StructSize = sizeof(PluginInfo);
  info->PluginMenu.Guids = &c_plugin_menu_guid;
  info->PluginMenu.Strings = plugin_menu;
  info->PluginMenu.Count = ARRAYSIZE(plugin_menu);
  info->PluginConfig.Guids = &c_plugin_config_guid;
  info->PluginConfig.Strings = config_menu;
  info->PluginConfig.Count = ARRAYSIZE(config_menu);
  info->CommandPrefix = g_plugin_prefix.c_str();
  FAR_ERROR_HANDLER_END(return, return, false);
}

HANDLE WINAPI AnalyseW(const AnalyseInfo* info) {
  FAR_ERROR_HANDLER_BEGIN;
  if (info->FileName == nullptr) {
    if (!g_options.handle_create)
      FAIL(E_ABORT);
    return new Archives();
  }
  else {
    GUARD(g_detect_next_time = triUndef);
    OpenOptions options;
    options.arc_path = info->FileName;
    options.arc_types = ArcAPI::formats().get_arc_types();
    if (g_detect_next_time == triUndef) {
      options.detect = false;
      if (!g_options.handle_commands)
        FAIL(E_ABORT);
      bool pgdn = (info->OpMode & OPM_PGDN) != 0;
      if (!pgdn || g_options.pgdn_masks) {
        if (g_options.use_include_masks && !Far::match_masks(extract_file_name(info->FileName), g_options.include_masks))
          FAIL(E_ABORT);
        if (g_options.use_exclude_masks && Far::match_masks(extract_file_name(info->FileName), g_options.exclude_masks))
          FAIL(E_ABORT);
      }
      if ((g_options.use_enabled_formats || g_options.use_disabled_formats) && (pgdn ? g_options.pgdn_formats : true)) {
        set<wstring> enabled_formats;
        if (g_options.use_enabled_formats) {
          list<wstring> name_list = split(upcase(g_options.enabled_formats), L',');
          copy(name_list.cbegin(), name_list.cend(), inserter(enabled_formats, enabled_formats.begin()));
        }
        set<wstring> disabled_formats;
        if (g_options.use_disabled_formats) {
          list<wstring> name_list = split(upcase(g_options.disabled_formats), L',');
          copy(name_list.cbegin(), name_list.cend(), inserter(disabled_formats, disabled_formats.begin()));
        }

        const ArcFormats& arc_formats = ArcAPI::formats();
        ArcTypes::iterator arc_type = options.arc_types.begin();
        while (arc_type != options.arc_types.end()) {
          wstring arc_name = upcase(arc_formats.at(*arc_type).name);
          if (g_options.use_enabled_formats && enabled_formats.count(arc_name) == 0)
            arc_type = options.arc_types.erase(arc_type);
          else if (g_options.use_disabled_formats && disabled_formats.count(arc_name) != 0)
            arc_type = options.arc_types.erase(arc_type);
          else
            arc_type++;
        }

        if (options.arc_types.empty())
          FAIL(E_ABORT);
      }
    }
    else
      options.detect = g_detect_next_time == triTrue;

    unique_ptr<Archives> archives(Archive::open(options));
    if (archives->empty())
      FAIL(E_ABORT);
    return archives.release();
  }
  FAR_ERROR_HANDLER_END(return nullptr, return nullptr, true);
}

void WINAPI CloseAnalyseW(const CloseAnalyseInfo* info) {
  delete static_cast<Archives*>(info->Handle);
}

HANDLE WINAPI OpenW(const OpenInfo* info) {
  FAR_ERROR_HANDLER_BEGIN;
  if (info->OpenFrom == OPEN_PLUGINSMENU) {
    Far::MenuItems menu_items;
    unsigned open_menu_id = menu_items.add(Far::get_msg(MSG_MENU_OPEN));
    unsigned detect_menu_id = menu_items.add(Far::get_msg(MSG_MENU_DETECT));
    unsigned create_menu_id = menu_items.add(Far::get_msg(MSG_MENU_CREATE));
    unsigned extract_menu_id = menu_items.add(Far::get_msg(MSG_MENU_EXTRACT));
    unsigned test_menu_id = menu_items.add(Far::get_msg(MSG_MENU_TEST));
    unsigned sfx_convert_menu_id = menu_items.add(Far::get_msg(MSG_MENU_SFX_CONVERT));
    unsigned item = Far::menu(c_main_menu_guid, Far::get_msg(MSG_PLUGIN_NAME), menu_items, L"Contents");
    if (item == open_menu_id || item == detect_menu_id) {
      OpenOptions options;
      options.detect = item == detect_menu_id;
      PanelInfo panel_info;
      if (!Far::get_panel_info(PANEL_ACTIVE, panel_info))
        return nullptr;
      Far::PanelItem panel_item = Far::get_current_panel_item(PANEL_ACTIVE);
      if (panel_item.file_attributes & FILE_ATTRIBUTE_DIRECTORY)
        return nullptr;
      if (!Far::is_real_file_panel(panel_info)) {
        if ((panel_item.file_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
          Far::post_macro(L"CtrlPgDn");
          g_detect_next_time = options.detect ? triTrue : triFalse;
        }
        return nullptr;
      }
      options.arc_path = add_trailing_slash(Far::get_panel_dir(PANEL_ACTIVE)) + panel_item.file_name;
      options.arc_types = ArcAPI::formats().get_arc_types();
      return Plugin::open(*Archive::open(options));
    }
    else if (item == create_menu_id) {
      Plugin::create_archive();
    }
    else if (item == extract_menu_id || item == test_menu_id || item == sfx_convert_menu_id) {
      PanelInfo panel_info;
      if (!Far::get_panel_info(PANEL_ACTIVE, panel_info))
        return nullptr;
      if (!Far::is_real_file_panel(panel_info))
        return nullptr;
      vector<wstring> file_list;
      file_list.reserve(panel_info.SelectedItemsNumber);
      wstring dir = Far::get_panel_dir(PANEL_ACTIVE);
      for (size_t i = 0; i < panel_info.SelectedItemsNumber; i++) {
        Far::PanelItem panel_item = Far::get_selected_panel_item(PANEL_ACTIVE, i);
        if ((panel_item.file_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
          wstring file_path = add_trailing_slash(dir) + panel_item.file_name;
          file_list.push_back(file_path);
        }
      }
      if (file_list.empty())
        return nullptr;
      if (item == extract_menu_id)
        Plugin::bulk_extract(file_list);
      else if (item == test_menu_id)
        Plugin::bulk_test(file_list);
      else if (item == sfx_convert_menu_id) {
        Plugin::convert_to_sfx(file_list);
      }
    }
  }
  else if (info->OpenFrom == OPEN_COMMANDLINE) {
    try {
      CommandArgs cmd_args = parse_command(reinterpret_cast<OpenCommandLineInfo*>(info->Data)->CommandLine);
      switch (cmd_args.cmd) {
      case cmdOpen: {
        OpenOptions options = parse_open_command(cmd_args).options;
        options.arc_path = Far::get_absolute_path(options.arc_path);
        return Plugin::open(*Archive::open(options));
      }
      case cmdCreate:
      case cmdUpdate:
        Plugin::cmdline_update(parse_update_command(cmd_args));
        break;
      case cmdExtract:
        Plugin::cmdline_extract(parse_extract_command(cmd_args));
        break;
      case cmdTest:
        Plugin::cmdline_test(parse_test_command(cmd_args));
        break;
      }
    }
    catch (const Error& e) {
      if (e.code == E_BAD_FORMAT)
        Far::open_help(L"Prefix");
      else
        throw;
    }
  }
  else if (info->OpenFrom == OPEN_ANALYSE) {
    const OpenAnalyseInfo* oai = reinterpret_cast<const OpenAnalyseInfo*>(info->Data);
    unique_ptr<Archives> archives(static_cast<Archives*>(oai->Handle));
    if (archives->empty()) {
      return new Plugin();
    }
    else {
      return Plugin::open(*archives);
    }
  }
  else if (info->OpenFrom == OPEN_SHORTCUT) {
    const OpenShortcutInfo* osi = reinterpret_cast<const OpenShortcutInfo*>(info->Data);
    OpenOptions options;
    options.arc_path = osi->HostFile;
    options.arc_types = ArcAPI::formats().get_arc_types();
    options.detect = true;
    return Plugin::open(*Archive::open(options));
  }
  return nullptr;
  FAR_ERROR_HANDLER_END(return nullptr, return nullptr, false);
}

void WINAPI ClosePanelW(const struct ClosePanelInfo* info) {
  FAR_ERROR_HANDLER_BEGIN;
  Plugin* plugin = reinterpret_cast<Plugin*>(info->hPanel);
  IGNORE_ERRORS(plugin->close());
  delete plugin;
  FAR_ERROR_HANDLER_END(return, return, true);
}

void WINAPI GetOpenPanelInfoW(OpenPanelInfo* info) {
  FAR_ERROR_HANDLER_BEGIN;
  reinterpret_cast<Plugin*>(info->hPanel)->info(info);
  FAR_ERROR_HANDLER_END(return, return, false);
}

intptr_t WINAPI SetDirectoryW(const SetDirectoryInfo* info) {
  FAR_ERROR_HANDLER_BEGIN;
  reinterpret_cast<Plugin*>(info->hPanel)->set_dir(info->Dir);
  return TRUE;
  FAR_ERROR_HANDLER_END(return FALSE, return FALSE, (info->OpMode & (OPM_SILENT | OPM_FIND)) != 0);
}

intptr_t WINAPI GetFindDataW(GetFindDataInfo* info) {
  FAR_ERROR_HANDLER_BEGIN;
  reinterpret_cast<Plugin*>(info->hPanel)->list(&info->PanelItem, &info->ItemsNumber);
  return TRUE;
  FAR_ERROR_HANDLER_END(return FALSE, return FALSE, (info->OpMode & (OPM_SILENT | OPM_FIND)) != 0);
}

void WINAPI FreeFindDataW(const FreeFindDataInfo* info) {
  FAR_ERROR_HANDLER_BEGIN;
  delete[] info->PanelItem;
  FAR_ERROR_HANDLER_END(return, return, false);
}

intptr_t WINAPI GetFilesW(GetFilesInfo *info) {
  FAR_ERROR_HANDLER_BEGIN
  reinterpret_cast<Plugin*>(info->hPanel)->get_files(info->PanelItem, info->ItemsNumber, info->Move, &info->DestPath, info->OpMode);
  return 1;
  FAR_ERROR_HANDLER_END(return 0, return -1, (info->OpMode & (OPM_FIND | OPM_QUICKVIEW)) != 0);
}

intptr_t WINAPI PutFilesW(const PutFilesInfo* info) {
  FAR_ERROR_HANDLER_BEGIN;
  reinterpret_cast<Plugin*>(info->hPanel)->put_files(info->PanelItem, info->ItemsNumber, info->Move, info->SrcPath, info->OpMode);
  return 2;
  FAR_ERROR_HANDLER_END(return 0, return -1, (info->OpMode & OPM_FIND) != 0);
}

intptr_t WINAPI DeleteFilesW(const DeleteFilesInfo* info) {
  FAR_ERROR_HANDLER_BEGIN;
  reinterpret_cast<Plugin*>(info->hPanel)->delete_files(info->PanelItem, info->ItemsNumber, info->OpMode);
  return TRUE;
  FAR_ERROR_HANDLER_END(return FALSE, return FALSE, (info->OpMode & OPM_SILENT) != 0);
}

intptr_t WINAPI MakeDirectoryW(MakeDirectoryInfo* info) {
  FAR_ERROR_HANDLER_BEGIN;
  reinterpret_cast<Plugin*>(info->hPanel)->create_dir(&info->Name, info->OpMode);
  return 1;
  FAR_ERROR_HANDLER_END(return -1, return -1, (info->OpMode & OPM_SILENT) != 0);
}

intptr_t WINAPI ProcessHostFileW(const ProcessHostFileInfo* info) {
  FAR_ERROR_HANDLER_BEGIN;
  Far::MenuItems menu_items;
  menu_items.add(Far::get_msg(MSG_TEST_MENU));
  int item = Far::menu(c_arccmd_menu_guid, Far::get_msg(MSG_PLUGIN_NAME), menu_items);
  if (item == 0)
    reinterpret_cast<Plugin*>(info->hPanel)->test_files(info->PanelItem, info->ItemsNumber, info->OpMode);
  return TRUE;
  FAR_ERROR_HANDLER_END(return FALSE, return FALSE, (info->OpMode & OPM_SILENT) != 0);
}

intptr_t WINAPI ProcessPanelInputW(const struct ProcessPanelInputInfo* info) {
  FAR_ERROR_HANDLER_BEGIN;
  if (info->Rec.EventType == KEY_EVENT) {
    const KEY_EVENT_RECORD& key_event = info->Rec.Event.KeyEvent;
    if ((key_event.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0 && key_event.wVirtualKeyCode == 'A') {
      reinterpret_cast<Plugin*>(info->hPanel)->show_attr();
      return TRUE;
    }
    else if ((key_event.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0 && key_event.wVirtualKeyCode == VK_F6) {
      reinterpret_cast<Plugin*>(info->hPanel)->extract();
      return TRUE;
    }
  }
  return FALSE;
  FAR_ERROR_HANDLER_END(return FALSE, return FALSE, false);
}

intptr_t WINAPI ConfigureW(const struct ConfigureInfo* info) {
  FAR_ERROR_HANDLER_BEGIN;
  PluginSettings settings;
  settings.handle_create = g_options.handle_create;
  settings.handle_commands = g_options.handle_commands;
  settings.own_panel_view_mode = g_options.own_panel_view_mode;
  settings.use_include_masks = g_options.use_include_masks;
  settings.include_masks = g_options.include_masks;
  settings.use_exclude_masks = g_options.use_exclude_masks;
  settings.exclude_masks = g_options.exclude_masks;
  settings.pgdn_masks = g_options.pgdn_masks;
  settings.use_enabled_formats = g_options.use_enabled_formats;
  settings.enabled_formats = g_options.enabled_formats;
  settings.use_disabled_formats = g_options.use_disabled_formats;
  settings.disabled_formats = g_options.disabled_formats;
  settings.pgdn_formats = g_options.pgdn_formats;
  if (settings_dialog(settings)) {
    g_options.handle_create = settings.handle_create;
    g_options.handle_commands = settings.handle_commands;
    g_options.own_panel_view_mode = settings.own_panel_view_mode;
    g_options.use_include_masks = settings.use_include_masks;
    g_options.include_masks = settings.include_masks;
    g_options.use_exclude_masks = settings.use_exclude_masks;
    g_options.exclude_masks = settings.exclude_masks;
    g_options.pgdn_masks = settings.pgdn_masks;
    g_options.use_enabled_formats = settings.use_enabled_formats;
    g_options.enabled_formats = settings.enabled_formats;
    g_options.use_disabled_formats = settings.use_disabled_formats;
    g_options.disabled_formats = settings.disabled_formats;
    g_options.pgdn_formats = settings.pgdn_formats;
    g_options.save();
    return TRUE;
  }
  else
    return FALSE;
  FAR_ERROR_HANDLER_END(return FALSE, return FALSE, false);
}

void WINAPI ExitFARW(ExitInfo* Info) {
  ArcAPI::free();
}
