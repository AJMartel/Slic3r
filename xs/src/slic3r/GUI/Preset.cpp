//#undef NDEBUG
#include <cassert>

#include "Preset.hpp"

#include <fstream>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <boost/nowide/cenv.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/locale.hpp>

#include <wx/image.h>
#include <wx/choice.h>
#include <wx/bmpcbox.h>
#include <wx/wupdlock.h>

#include "../../libslic3r/libslic3r.h"
#include "../../libslic3r/Utils.hpp"

namespace Slic3r {

ConfigFileType guess_config_file_type(const boost::property_tree::ptree &tree)
{
    size_t app_config   = 0;
    size_t bundle       = 0;
    size_t config       = 0;
    for (const boost::property_tree::ptree::value_type &v : tree) {
        if (v.second.empty()) {
            if (v.first == "background_processing" ||
                v.first == "last_output_path" ||
                v.first == "no_controller" ||
                v.first == "no_defaults")
                ++ app_config;
            else if (v.first == "nozzle_diameter" ||
                v.first == "filament_diameter")
                ++ config;
        } else if (boost::algorithm::starts_with(v.first, "print:") ||
            boost::algorithm::starts_with(v.first, "filament:") ||
            boost::algorithm::starts_with(v.first, "printer:") ||
            v.first == "settings")
            ++ bundle;
        else if (v.first == "presets") {
            ++ app_config;
            ++ bundle;
        } else if (v.first == "recent") {
            for (auto &kvp : v.second)
                if (kvp.first == "config_directory" || kvp.first == "skein_directory")
                    ++ app_config;
        }
    }
    return (app_config > bundle && app_config > config) ? CONFIG_FILE_TYPE_APP_CONFIG :
           (bundle > config) ? CONFIG_FILE_TYPE_CONFIG_BUNDLE : CONFIG_FILE_TYPE_CONFIG;
}

// Suffix to be added to a modified preset name in the combo box.
static std::string g_suffix_modified = " (modified)";
const std::string& Preset::suffix_modified()
{
    return g_suffix_modified;
}
// Remove an optional "(modified)" suffix from a name.
// This converts a UI name to a unique preset identifier.
std::string Preset::remove_suffix_modified(const std::string &name)
{
    return boost::algorithm::ends_with(name, g_suffix_modified) ?
        name.substr(0, name.size() - g_suffix_modified.size()) :
        name;
}

void Preset::set_num_extruders(DynamicPrintConfig &config, unsigned int num_extruders)
{
    const auto &defaults = FullPrintConfig::defaults();
    for (const std::string &key : Preset::nozzle_options()) {
        auto *opt = config.option(key, false);
        assert(opt != nullptr);
        assert(opt->is_vector());
        static_cast<ConfigOptionVectorBase*>(opt)->resize(num_extruders, defaults.option(key));
    }
}

// Update new extruder fields at the printer profile.
void Preset::normalize(DynamicPrintConfig &config)
{
    auto *nozzle_diameter = dynamic_cast<const ConfigOptionFloats*>(config.option("nozzle_diameter"));
    if (nozzle_diameter != nullptr)
        // Loaded the Printer settings. Verify, that all extruder dependent values have enough values.
        set_num_extruders(config, (unsigned int)nozzle_diameter->values.size());
    if (config.option("filament_diameter") != nullptr) {
        // This config contains single or multiple filament presets.
        // Ensure that the filament preset vector options contain the correct number of values.
        size_t n = (nozzle_diameter == nullptr) ? 1 : nozzle_diameter->values.size();
        const auto &defaults = FullPrintConfig::defaults();
        for (const std::string &key : Preset::filament_options()) {
			if (key == "compatible_printers")
				continue;
            auto *opt = config.option(key, false);
            assert(opt != nullptr);
            assert(opt->is_vector());
            if (opt != nullptr && opt->is_vector())
                static_cast<ConfigOptionVectorBase*>(opt)->resize(n, defaults.option(key));
        }
    }
}

// Load a config file, return a C++ class Slic3r::DynamicPrintConfig with $keys initialized from the config file.
// In case of a "default" config item, return the default values.
DynamicPrintConfig& Preset::load(const std::vector<std::string> &keys)
{
    // Set the configuration from the defaults.
    Slic3r::FullPrintConfig defaults;
    this->config.apply_only(defaults, keys.empty() ? defaults.keys() : keys);
    if (! this->is_default) {
        // Load the preset file, apply preset values on top of defaults.
        try {
            this->config.load_from_ini(this->file);
            Preset::normalize(this->config);
        } catch (const std::ifstream::failure &err) {
            throw std::runtime_error(std::string("The selected preset cannot be loaded: ") + this->file + "\n\tReason: " + err.what());
        } catch (const std::runtime_error &err) {
            throw std::runtime_error(std::string("Failed loading the preset file: ") + this->file + "\n\tReason: " + err.what());
        }
    }
    this->loaded = true;
    return this->config;
}

void Preset::save()
{
    this->config.save(this->file);
}

// Return a label of this preset, consisting of a name and a "(modified)" suffix, if this preset is dirty.
std::string Preset::label() const
{
    return this->name + (this->is_dirty ? g_suffix_modified : "");
}

bool Preset::is_compatible_with_printer(const std::string &active_printer) const
{
    auto *compatible_printers = dynamic_cast<const ConfigOptionStrings*>(this->config.option("compatible_printers"));
    return this->is_default || active_printer.empty() ||
        compatible_printers == nullptr || compatible_printers->values.empty() ||
        std::find(compatible_printers->values.begin(), compatible_printers->values.end(), active_printer) != 
            compatible_printers->values.end();
}

bool Preset::update_compatible_with_printer(const std::string &active_printer)
{
    return this->is_compatible = is_compatible_with_printer(active_printer);
}

const std::vector<std::string>& Preset::print_options()
{    
    static std::vector<std::string> s_opts {
        "layer_height", "first_layer_height", "perimeters", "spiral_vase", "top_solid_layers", "bottom_solid_layers", 
        "extra_perimeters", "ensure_vertical_shell_thickness", "avoid_crossing_perimeters", "thin_walls", "overhangs", 
        "seam_position", "external_perimeters_first", "fill_density", "fill_pattern", "external_fill_pattern", 
        "infill_every_layers", "infill_only_where_needed", "solid_infill_every_layers", "fill_angle", "bridge_angle", 
        "solid_infill_below_area", "only_retract_when_crossing_perimeters", "infill_first", "max_print_speed", 
        "max_volumetric_speed", "max_volumetric_extrusion_rate_slope_positive", "max_volumetric_extrusion_rate_slope_negative", 
        "perimeter_speed", "small_perimeter_speed", "external_perimeter_speed", "infill_speed", "solid_infill_speed", 
        "top_solid_infill_speed", "support_material_speed", "support_material_xy_spacing", "support_material_interface_speed",
        "bridge_speed", "gap_fill_speed", "travel_speed", "first_layer_speed", "perimeter_acceleration", "infill_acceleration", 
        "bridge_acceleration", "first_layer_acceleration", "default_acceleration", "skirts", "skirt_distance", "skirt_height",
        "min_skirt_length", "brim_width", "support_material", "support_material_threshold", "support_material_enforce_layers", 
        "raft_layers", "support_material_pattern", "support_material_with_sheath", "support_material_spacing", 
        "support_material_synchronize_layers", "support_material_angle", "support_material_interface_layers", 
        "support_material_interface_spacing", "support_material_interface_contact_loops", "support_material_contact_distance", 
        "support_material_buildplate_only", "dont_support_bridges", "notes", "complete_objects", "extruder_clearance_radius", 
        "extruder_clearance_height", "gcode_comments", "output_filename_format", "post_process", "perimeter_extruder", 
        "infill_extruder", "solid_infill_extruder", "support_material_extruder", "support_material_interface_extruder", 
        "ooze_prevention", "standby_temperature_delta", "interface_shells", "extrusion_width", "first_layer_extrusion_width", 
        "perimeter_extrusion_width", "external_perimeter_extrusion_width", "infill_extrusion_width", "solid_infill_extrusion_width", 
        "top_infill_extrusion_width", "support_material_extrusion_width", "infill_overlap", "bridge_flow_ratio", "clip_multipart_objects", 
        "elefant_foot_compensation", "xy_size_compensation", "threads", "resolution", "wipe_tower", "wipe_tower_x", "wipe_tower_y",
        "wipe_tower_width", "wipe_tower_per_color_wipe",
        "compatible_printers"
    };
    return s_opts;
}

const std::vector<std::string>& Preset::filament_options()
{    
    static std::vector<std::string> s_opts {
        "filament_colour", "filament_diameter", "filament_type", "filament_soluble", "filament_notes", "filament_max_volumetric_speed", 
        "extrusion_multiplier", "filament_density", "filament_cost", "temperature", "first_layer_temperature", "bed_temperature", 
        "first_layer_bed_temperature", "fan_always_on", "cooling", "min_fan_speed", "max_fan_speed", "bridge_fan_speed", 
        "disable_fan_first_layers", "fan_below_layer_time", "slowdown_below_layer_time", "min_print_speed", "start_filament_gcode", 
        "end_filament_gcode",
        "compatible_printers"
    };
    return s_opts;
}

const std::vector<std::string>& Preset::printer_options()
{    
    static std::vector<std::string> s_opts;
    if (s_opts.empty()) {
        s_opts = {
            "bed_shape", "z_offset", "gcode_flavor", "use_relative_e_distances", "serial_port", "serial_speed", 
            "octoprint_host", "octoprint_apikey", "use_firmware_retraction", "use_volumetric_e", "variable_layer_height", 
            "single_extruder_multi_material", "start_gcode", "end_gcode", "before_layer_gcode", "layer_gcode", "toolchange_gcode",
            "between_objects_gcode", "printer_notes"
        };
        s_opts.insert(s_opts.end(), Preset::nozzle_options().begin(), Preset::nozzle_options().end());
    }
    return s_opts;
}

const std::vector<std::string>& Preset::nozzle_options()
{
    // ConfigOptionFloats, ConfigOptionPercents, ConfigOptionBools, ConfigOptionStrings
    static std::vector<std::string> s_opts {
        "nozzle_diameter", "min_layer_height", "max_layer_height", "extruder_offset",
        "retract_length", "retract_lift", "retract_lift_above", "retract_lift_below", "retract_speed", "deretract_speed",
        "retract_before_wipe", "retract_restart_extra", "retract_before_travel", "wipe",
        "retract_layer_change", "retract_length_toolchange", "retract_restart_extra_toolchange", "extruder_colour"
    };
    return s_opts;
}

PresetCollection::PresetCollection(Preset::Type type, const std::vector<std::string> &keys) :
    m_type(type),
    m_edited_preset(type, "", false),
    m_idx_selected(0),
    m_bitmap_main_frame(new wxBitmap)
{
    // Insert just the default preset.
    m_presets.emplace_back(Preset(type, "- default -", true));
    m_presets.front().load(keys);
    m_edited_preset.config.apply(m_presets.front().config);
}

PresetCollection::~PresetCollection()
{
    delete m_bitmap_main_frame;
    m_bitmap_main_frame = nullptr;
}

// Load all presets found in dir_path.
// Throws an exception on error.
void PresetCollection::load_presets(const std::string &dir_path, const std::string &subdir)
{
	boost::filesystem::path dir = boost::filesystem::canonical(boost::filesystem::path(dir_path) / subdir).make_preferred();
	m_dir_path = dir.string();
    m_presets.erase(m_presets.begin()+1, m_presets.end());
    t_config_option_keys keys = this->default_preset().config.keys();
    std::string errors_cummulative;
	for (auto &dir_entry : boost::filesystem::directory_iterator(dir))
        if (boost::filesystem::is_regular_file(dir_entry.status()) && boost::algorithm::iends_with(dir_entry.path().filename().string(), ".ini")) {
            std::string name = dir_entry.path().filename().string();
            // Remove the .ini suffix.
            name.erase(name.size() - 4);
            try {
                Preset preset(m_type, name, false);
                preset.file = dir_entry.path().string();
                preset.load(keys);
                m_presets.emplace_back(preset);
            } catch (const std::runtime_error &err) {
                errors_cummulative += err.what();
                errors_cummulative += "\n";
			}
        }
    std::sort(m_presets.begin() + 1, m_presets.end());
    this->select_preset(first_visible_idx());
    if (! errors_cummulative.empty())
        throw std::runtime_error(errors_cummulative);
}

// Load a preset from an already parsed config file, insert it into the sorted sequence of presets
// and select it, losing previous modifications.
Preset& PresetCollection::load_preset(const std::string &path, const std::string &name, const DynamicPrintConfig &config, bool select)
{
    DynamicPrintConfig cfg(this->default_preset().config);
    cfg.apply_only(config, cfg.keys(), true);
    return this->load_preset(path, name, std::move(cfg));
}

Preset& PresetCollection::load_preset(const std::string &path, const std::string &name, DynamicPrintConfig &&config, bool select)
{
    Preset key(m_type, name);
    auto it = std::lower_bound(m_presets.begin(), m_presets.end(), key);
    if (it == m_presets.end() || it->name != name)
        it = m_presets.emplace(it, Preset(m_type, name, false));
    Preset &preset = *it;
    preset.file = path;
    preset.config = std::move(config);
    preset.loaded = true;
    preset.is_dirty = false;
    if (select)
        this->select_preset_by_name(name, true);
    return preset;
}

void PresetCollection::save_current_preset(const std::string &new_name)
{
	// 1) Find the preset with a new_name or create a new one,
	// initialize it with the edited config.
    Preset key(m_type, new_name, false);
    auto it = std::lower_bound(m_presets.begin(), m_presets.end(), key);
    if (it != m_presets.end() && it->name == key.name) {
        // Preset with the same name found.
        Preset &preset = *it;
        if (preset.is_default)
            // Cannot overwrite the default preset.
            return;
        // Overwriting an existing preset.
        preset.config = std::move(m_edited_preset.config);
    } else {
        // Creating a new preset.
		Preset &preset = *m_presets.insert(it, m_edited_preset);
        std::string file_name = new_name;
        if (! boost::iends_with(file_name, ".ini"))
            file_name += ".ini";
        preset.name = new_name;
		preset.file = (boost::filesystem::path(m_dir_path) / file_name).make_preferred().string();
    }
	// 2) Activate the saved preset.
	this->select_preset_by_name(new_name, true);
	// 2) Store the active preset to disk.
	this->get_selected_preset().save();
}

void PresetCollection::delete_current_preset()
{
    const Preset &selected = this->get_selected_preset();
    if (selected.is_default)
        return;
	if (! selected.is_external) {
		// Erase the preset file.
		boost::nowide::remove(selected.file.c_str());
	}
    // Remove the preset from the list.
    m_presets.erase(m_presets.begin() + m_idx_selected);
    // Find the next visible preset.
    size_t new_selected_idx = m_idx_selected;
    if (new_selected_idx < m_presets.size())
        for (; new_selected_idx < m_presets.size() && ! m_presets[new_selected_idx].is_visible; ++ new_selected_idx) ;
    if (new_selected_idx == m_presets.size())
		for (--new_selected_idx; new_selected_idx > 0 && !m_presets[new_selected_idx].is_visible; --new_selected_idx);
    this->select_preset(new_selected_idx);
}

bool PresetCollection::load_bitmap_default(const std::string &file_name)
{
    return m_bitmap_main_frame->LoadFile(wxString::FromUTF8(Slic3r::var(file_name).c_str()), wxBITMAP_TYPE_PNG);
}

// Return a preset by its name. If the preset is active, a temporary copy is returned.
// If a preset is not found by its name, null is returned.
Preset* PresetCollection::find_preset(const std::string &name, bool first_visible_if_not_found)
{
    Preset key(m_type, name, false);
    auto it = std::lower_bound(m_presets.begin(), m_presets.end(), key);
    // Ensure that a temporary copy is returned if the preset found is currently selected.
    return (it != m_presets.end() && it->name == key.name) ? &this->preset(it - m_presets.begin()) : 
        first_visible_if_not_found ? &this->first_visible() : nullptr;
}

// Return index of the first visible preset. Certainly at least the '- default -' preset shall be visible.
size_t PresetCollection::first_visible_idx() const
{
    size_t idx = m_default_suppressed ? 1 : 0;
    for (; idx < this->m_presets.size(); ++ idx)
        if (m_presets[idx].is_visible)
            break;
    if (idx == this->m_presets.size())
        idx = 0;
    return idx;
}

// Return index of the first compatible preset. Certainly at least the '- default -' preset shall be compatible.
size_t PresetCollection::first_compatible_idx() const
{
    size_t idx = m_default_suppressed ? 1 : 0;
    for (; idx < this->m_presets.size(); ++ idx)
        if (m_presets[idx].is_compatible)
            break;
    if (idx == this->m_presets.size())
        idx = 0;
    return idx;
}

void PresetCollection::set_default_suppressed(bool default_suppressed)
{
    if (m_default_suppressed != default_suppressed) {
        m_default_suppressed = default_suppressed;
        m_presets.front().is_visible = ! default_suppressed || (m_presets.size() > 1 && m_idx_selected > 0);
    }
}

void PresetCollection::update_compatible_with_printer(const std::string &active_printer, bool select_other_if_incompatible)
{
    size_t num_visible = 0;
    for (size_t idx_preset = 1; idx_preset < m_presets.size(); ++ idx_preset) {
        bool    selected        = idx_preset == m_idx_selected;
        Preset &preset_selected = m_presets[idx_preset];
        Preset &preset_edited   = selected ? m_edited_preset : preset_selected;
        if (preset_edited.update_compatible_with_printer(active_printer))
            // Mark compatible presets as visible.
            preset_selected.is_visible = true;
        else if (selected && select_other_if_incompatible) {
            preset_selected.is_visible = false;
            m_idx_selected = (size_t)-1;
        }
        if (selected)
            preset_selected.is_compatible = preset_edited.is_compatible;
        if (preset_selected.is_visible)
            ++ num_visible;
    }
    if (m_idx_selected == (size_t)-1)
        // Find some other visible preset.
        this->select_preset(first_visible_idx());
    else if (num_visible == 0)
        // Show the "-- default --" preset.
        m_presets.front().is_visible = true;
}

// Save the preset under a new name. If the name is different from the old one,
// a new preset is stored into the list of presets.
// All presets are marked as not modified and the new preset is activated.
//void PresetCollection::save_current_preset(const std::string &new_name);

// Delete the current preset, activate the first visible preset.
//void PresetCollection::delete_current_preset();

// Update the wxChoice UI component from this list of presets.
// Hide the 
void PresetCollection::update_platter_ui(wxBitmapComboBox *ui)
{
    if (ui == nullptr)
        return;
    // Otherwise fill in the list from scratch.
    ui->Freeze();
    ui->Clear();
    for (size_t i = this->m_presets.front().is_visible ? 0 : 1; i < this->m_presets.size(); ++ i) {
        const Preset &preset = this->m_presets[i];
        if (! preset.is_visible || (! preset.is_compatible && i != m_idx_selected))
            continue;
        const wxBitmap *bmp = (i == 0 || preset.is_compatible) ? m_bitmap_main_frame : m_bitmap_incompatible;
        ui->Append(wxString::FromUTF8((preset.name + (preset.is_dirty ? g_suffix_modified : "")).c_str()),
            (bmp == 0) ? (m_bitmap_main_frame ? *m_bitmap_main_frame : wxNullBitmap) : *bmp);
		if (i == m_idx_selected)
            ui->SetSelection(ui->GetCount() - 1);
    }
    ui->Thaw();
}

void PresetCollection::update_tab_ui(wxBitmapComboBox *ui, bool show_incompatible)
{
    if (ui == nullptr)
        return;
    ui->Freeze();
    ui->Clear();
    for (size_t i = this->m_presets.front().is_visible ? 0 : 1; i < this->m_presets.size(); ++ i) {
        const Preset &preset = this->m_presets[i];
        if (! show_incompatible && ! preset.is_compatible && i != m_idx_selected)
            continue;
        const wxBitmap *bmp = preset.is_compatible ? m_bitmap_compatible : m_bitmap_incompatible;
        ui->Append(wxString::FromUTF8((preset.name + (preset.is_dirty ? g_suffix_modified : "")).c_str()),
            (bmp == 0) ? (m_bitmap_main_frame ? *m_bitmap_main_frame : wxNullBitmap) : *bmp);
		if (i == m_idx_selected)
            ui->SetSelection(ui->GetCount() - 1);
    }
    ui->Thaw();
}

// Update a dirty floag of the current preset, update the labels of the UI component accordingly.
// Return true if the dirty flag changed.
bool PresetCollection::update_dirty_ui(wxBitmapComboBox *ui)
{
    wxWindowUpdateLocker noUpdates(ui);
    // 1) Update the dirty flag of the current preset.
    bool was_dirty = this->get_selected_preset().is_dirty;
    bool is_dirty  = current_is_dirty();
    this->get_selected_preset().is_dirty = is_dirty;
    this->get_edited_preset().is_dirty = is_dirty;
    // 2) Update the labels.
    for (unsigned int ui_id = 0; ui_id < ui->GetCount(); ++ ui_id) {
        std::string   old_label    = ui->GetString(ui_id).utf8_str().data();
        std::string   preset_name  = Preset::remove_suffix_modified(old_label);
        const Preset *preset       = this->find_preset(preset_name, false);
        assert(preset != nullptr);
        std::string new_label = preset->is_dirty ? preset->name + g_suffix_modified : preset->name;
        if (old_label != new_label)
            ui->SetString(ui_id, wxString::FromUTF8(new_label.c_str()));
    }
    return was_dirty != is_dirty;
}

Preset& PresetCollection::select_preset(size_t idx)
{
    for (Preset &preset : m_presets)
        preset.is_dirty = false;
    if (idx >= m_presets.size())
        idx = first_visible_idx();
    m_idx_selected = idx;
    m_edited_preset = m_presets[idx];
    m_presets.front().is_visible = ! m_default_suppressed || m_idx_selected == 0;
    return m_presets[idx];
}

bool PresetCollection::select_preset_by_name(const std::string &name_w_suffix, bool force)
{   
    std::string name = Preset::remove_suffix_modified(name_w_suffix);
    // 1) Try to find the preset by its name.
    Preset key(m_type, name, false);
    auto it = std::lower_bound(m_presets.begin(), m_presets.end(), key);
    size_t idx = 0;
    if (it != m_presets.end() && it->name == key.name)
        // Preset found by its name.
        idx = it - m_presets.begin();
    else {
        // Find the first visible preset.
        for (size_t i = m_default_suppressed ? 1 : 0; i < m_presets.size(); ++ i)
            if (m_presets[i].is_visible) {
                idx = i;
                break;
            }
        // If the first visible preset was not found, return the 0th element, which is the default preset.
    }

    // 2) Select the new preset.
    if (m_idx_selected != idx || force) {
        this->select_preset(idx);
        return true;
    }

    return false;
}

std::string PresetCollection::name() const
{
    switch (this->type()) {
    case Preset::TYPE_PRINT:    return "print";
    case Preset::TYPE_FILAMENT: return "filament";
    case Preset::TYPE_PRINTER:  return "printer";
    default:                    return "invalid";
    }
}

} // namespace Slic3r
