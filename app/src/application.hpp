#pragma once

#include <vector>
#include <unordered_map>
#include <filesystem>
#include <optional>
#include <string>
#include <chrono>
#include <functional>

#include <imgui.h>
#include <alfred/real_synthesizer.hpp>

#include "video.hpp"
#include "sequencer.hpp"
#include "composition.hpp"
#include "ui.hpp"
#include "data.hpp"
#include "task.hpp"
#include "image.hpp"
#include "flat_set.hpp"
#include "preset.hpp"

namespace alfred::application {
    using MeasureIter = seq::MeasureIter;
    using NoteIter = seq::NoteIter;
    using ProvenanceNote = seq::ProvenanceNote<>;

    class HoveredNote {
    public:
        HoveredNote() = default;
        HoveredNote(syn::NoteId id, MeasureIter measure, std::uint32_t position, std::uint32_t global_position)
            : m_id(id), m_measure(measure), m_position(position), m_global_position(global_position) {}

        syn::NoteId id() const { return m_id; }
        MeasureIter measure() const { return m_measure; }
        std::uint32_t position() const { return m_position; }
        std::uint32_t global_position() const { return m_global_position; }
        std::uint32_t measure_position() const { return m_global_position - m_position; }
    private:
        syn::NoteId m_id {};
        MeasureIter m_measure;
        std::uint32_t m_position {};
        std::uint32_t m_global_position {};
    };

    struct Time {
        int minutes {};
        int seconds {};
        int deciseconds {};
    };

    struct Draw {
        ImDrawList* list {};
        ImVec2 origin;
        ImVec2 space;
        ImVec2 clamped_space;
    };

    struct CompositionHistory {
        struct Point {
            seq::Composition composition;
            ImVec2 camera;
        };

        using Stack = std::vector<Point>;

        Stack undo;
        Stack redo;
    };

    class Application : public video::Video {
    public:
        void on_start() override;
        void on_stop() override;
        void on_update() override;
        void on_imgui() override;
        void on_late_update() override;
        void on_event(const SDL_Event& event) override;
    private:
        void main_menu_bar();
        void main_menu_bar_file();
        void main_menu_bar_edit();
        void main_menu_bar_composition();
        void main_menu_bar_instrument();
        void main_menu_bar_options();
        void main_menu_bar_audio() const;
        void main_menu_bar_help();
        void keyboard() const;
        void keyboard_key(const Draw& draw, char key, float x, float y, int scancode) const;
        void instruments_and_synthesizer();
        void output();
        void output_indicator() const;
        void playback();
        void tools();
        void tools_measure();
        void tools_note();
        void composition();
        void composition_left(const Draw& draw) const;
        void composition_octaves(const Draw& draw) const;
        void composition_measures(const Draw& draw) const;
        void composition_measures_labels(const Draw& draw) const;
        void composition_notes(const Draw& draw) const;
        void composition_notes(const Draw& draw, syn::InstrumentId instrument, const seq::Notes& notes, float global_position_x) const;
        void composition_cursor(const Draw& draw) const;
        void composition_hover(const Draw& draw, const HoveredNote& hovered_note) const;
        void composition_pitch(const Draw& draw, const HoveredNote& hovered_note) const;
        void shortcuts();
        bool time_signature();
        bool dynamics();
        bool agogic();
        void composition_metadata();
        void composition_mixer();
        void create_instrument();
        static void create_instrument_envelope(ui::preset::Envelope& envelope);
        static void create_instrument_base(ui::preset::BasePreset& preset);
        void create_instrument_add();
        void create_instrument_pad();
        void create_instrument_buttons_add();
        void create_instrument_buttons_pad();
        void render_composition();
        void messages();
        void debug() const;
        static void window_menu(const char* name, bool& open, const std::function<void()>& window);

        void keyboard_input(unsigned int key, bool down);
        void composition_mouse_pressed(ImVec2 origin);
        void composition_camera(bool item_active, bool item_hovered, ImVec2 space);
        void toggle_select_measure(MeasureIter measure);
        void select_measure(MeasureIter measure);
        void append_measures();
        void insert_measure();
        void clear_measure();
        void delete_measure();
        void set_measure_dynamics();
        void set_measure_agogic();
        void set_measure_time_signature();
        std::optional<MeasureIter> hover_measure(ImVec2 position);
        std::optional<HoveredNote> hover_note(ImVec2 position);
        std::optional<NoteIter> select_note(const HoveredNote& hovered_note) const;
        std::optional<std::uint32_t> hover_position(ImVec2 position) const;
        void do_with_note(const HoveredNote& hovered_note);
        void delete_notes();
        void legato_notes();
        void shift_notes_up();
        void shift_notes_down();
        void shift_notes_left();
        void shift_notes_right();
        void add_delay_notes();
        void remove_delay_notes();

        void start_player();
        void stop_player();
        void seek_player(std::uint32_t position);
        void seek_player_to_start();
        void modify_composition();
        void modify_composition_metadata();
        void invalidate_composition();
        void reset_composition_flags();
        void reset_composition_selection();
        void reset_everything();
        void set_title_composition_not_saved() const;
        void set_title_composition_saved() const;
        static void set_color_scheme(ui::ColorScheme color_scheme);
        void set_scale(ui::Scale scale);
        float composition_width() const;
        ImVec2 composition_space(ImVec2 space) const;
        ImVec2 composition_mouse_position(ImVec2 origin) const;
        std_flat_set<syn::InstrumentId> active_instruments() const;
        bool point_x_in_camera_view(float point_x, float space_x) const;
        bool point_y_in_camera_view(float point_y, float space_y) const;
        static void readd_note(ProvenanceNote& provenance_note, const seq::Note& note);
        static void readd_note(syn::InstrumentId instrument, NoteIter note_iter, MeasureIter measure, const seq::Note& note);
        static void reset_note_legato(const ProvenanceNote& provenance_note);
        void reset_note_legato_previous_measure(MeasureIter measure) const;
        void play_note(syn::InstrumentId instrument, const seq::Note& note);

        static void note_to_string(syn::NoteId note, char* buffer);
        static bool keyboard_active();
        static float note_height(const seq::Note& note);
        static ImVec4 note_rectangle(const seq::Note& note);
        static const char* measure_label(char* buffer, long number);
        static void measure_properties(MeasureIter measure, const std::vector<seq::Measure>& measures, seq::TimeSignature& time_signature, seq::Dynamics& dynamics, seq::Agogic& agogic);
        static void set_dynamics(seq::Measure& measure, ui::Dynamics dynamics);
        static void set_dynamics(ui::Dynamics& dynamics, const seq::Measure& measure);
        static void set_agogic(seq::Measure& measure, ui::Agogic agogic);
        static void set_agogic(ui::Agogic& agogic, const seq::Measure& measure);
        static void set_time_signature(seq::Measure& measure, ui::TimeSignature time_signature);
        static void set_time_signature(ui::TimeSignature& time_signature, const seq::Measure& measure);
        static bool measure_empty(const seq::Measure& measure);
        static bool check_note_up_limit(const seq::Note& note);
        static bool check_note_down_limit(const seq::Note& note);
        static bool check_note_left_limit(const seq::Note& note);
        static bool check_note_right_limit(const seq::Note& note, const seq::Measure& measure);
        static bool notes_overlapping(const seq::Note& note1, const seq::Note& note2);
        static bool note_in_selection(NoteIter note, MeasureIter measure, const std::vector<ProvenanceNote>& selected_notes);
        std::optional<ProvenanceNote> check_note_has_next(const ProvenanceNote& provenance_note) const;
        std::optional<ProvenanceNote> check_note_has_previous(const ProvenanceNote& provenance_note) const;
        static Time elapsed_seconds_to_time(double elapsed_seconds);
        static seq::Value translate_value(ui::Value value);
        static preset::Envelope translate_envelope(const ui::preset::Envelope& envelope);
        static ui::preset::Envelope translate_envelope(const preset::Envelope& envelope);
        static preset::BasePreset translate_preset(const ui::preset::BasePreset& preset);
        static ui::preset::BasePreset translate_preset(const preset::BasePreset& preset);
        static preset::add::Preset translate_preset(const ui::preset::PresetAdd& preset);
        static ui::preset::PresetAdd translate_preset(const preset::add::Preset& preset);
        static preset::pad::Preset translate_preset(const ui::preset::PresetPad& preset);
        static ui::preset::PresetPad translate_preset(const preset::pad::Preset& preset);
        static const ImVec4& color(ImGuiCol color);
        static ImColor color_opacity(ImGuiCol color, float opacity);
        static void check_path_extension(const std::filesystem::path& path, const char* extension);

        struct FileDialogData {
            using Function = bool(Application::*)(std::filesystem::path);

            Application* self {};
            Function function {};
        };

        static void save_file_dialog(void* userdata, const char* const* filelist, int filter);
        static void open_file_dialog(void* userdata, const char* const* filelist, int filter);

        static void composition_write(const std::filesystem::path& path, const composition::Composition& composition);
        static void composition_read(const std::filesystem::path& path, composition::Composition& composition);
        bool composition_save(std::filesystem::path path);
        bool composition_save();
        bool composition_open(std::filesystem::path path);
        void composition_new();
        void composition_file_save();
        void composition_file_open();
        void composition_file_new();

        static void preset_write(const std::filesystem::path& path, const preset::add::Preset& preset);
        static void preset_write(const std::filesystem::path& path, const preset::pad::Preset& preset);
        static void preset_read(const std::filesystem::path& path, preset::add::Preset& preset);
        static void preset_read(const std::filesystem::path& path, preset::pad::Preset& preset);
        bool preset_save_add(std::filesystem::path path);
        bool preset_save_pad(std::filesystem::path path);
        bool preset_open_add(std::filesystem::path path);
        bool preset_open_pad(std::filesystem::path path);
        bool preset_open_add_silent(std::filesystem::path path, ui::preset::PresetAdd& preset_add) const;
        bool preset_open_pad_silent(std::filesystem::path path, ui::preset::PresetPad& preset_pad) const;
        void preset_file_save_add();
        void preset_file_save_pad();
        void preset_file_open_add();
        void preset_file_open_pad();

        void open_composition_metadata();
        void open_composition_mixer();
        void open_create_instrument(std::optional<ui::CreateInstrumentTab> create_instrument_tab = std::nullopt);
        void open_render_composition();

        void load_presets_from_disk();

        struct RenderCompositionParameters {
            synthesizer::VirtualSynthesizer synthesizer;
            std::filesystem::path file_path;
            composition::Composition composition;
            bool normalize {};
            float* render_progress {};
        };

        using NotifyMessage = std::function<void(std::string)>;

        void reset_render_composition();
        void start_render_composition();
        static void do_render_composition(const task::AsyncTask& task, task::TaskManager& task_manager, const NotifyMessage& notify_message, RenderCompositionParameters parameters);
        static std::size_t max_composition_voices(const seq::Composition& composition);
        static std::size_t optimal_composition_voices(const seq::Composition& composition);
        static void strip_composition_empty_instruments(seq::Composition& composition);
        void undo();
        void redo();
        void remember_composition();
        void keep_player_cursor_valid();
        void set_synthesizer_instrument_volumes(synthesizer::Synthesizer& synthesizer) const;
        static void set_synthesizer_instrument_volumes(synthesizer::Synthesizer& synthesizer, const composition::Composition& composition);
        static void reset_synthesizer_instrument_volumes(synthesizer::Synthesizer& synthesizer);
        void set_composition_instrument_colors();
        void reset_composition_instrument_colors();

        struct Message {
            std::string message;
            std::chrono::system_clock::time_point time {};
            std::uint32_t sequence {};
            float opacity = 1.0f;
        };

        void notify_message(std::string message) const;
        void update_messages();
        void erase_message(const Message& message);

        mutable struct {
            std::vector<Message> messages;
            std::uint32_t sequence {};
        } m_messages;

        data::Data m_data;
        task::TaskManager m_task_manager;

        syn::InstrumentId m_instrument {};
        syn::keyboard::Octave m_octave = syn::keyboard::OctaveFourth;

        ImVec2 m_composition_camera;
        MeasureIter m_composition_selected_measure;
        std::vector<ProvenanceNote> m_composition_selected_notes;
        std::filesystem::path m_composition_path;
        CompositionHistory m_composition_history;

        composition::Composition m_composition;
        synthesizer::RealSynthesizer m_synthesizer;
        seq::Player m_player;

        struct {
            bool metronome {};
            int tool = ui::ToolMeasure;
            int value = ui::ValueQuarter;
            int tuplet = ui::TupletNone;
            ui::Dynamics dynamics;
            ui::Agogic agogic;
            ui::TimeSignature time_signature;
            ui::Composition composition;
            ui::preset::PresetAdd preset_add;
            ui::preset::PresetPad preset_pad;
            ui::CreateInstrumentTab create_instrument_tab {};
            std::optional<ui::CreateInstrumentTab> create_instrument_tab_select;
            int octave = ui::OctaveFourth;
            int loudness = ui::LoudnessMezzoForte;
            int polyphony {};
            double volume {};
            double current_output_sample {};
            double past_output_sample_abs {};
            double smoothed_output_sample {};
            bool render_normalize = true;
            float render_progress {};
            char render_file_path[256] {};
            image::Texture texture_play;
            image::Texture texture_pause;
            image::Texture texture_rewind;
            std::unordered_map<syn::InstrumentId, ui::ColorIndex> colors;
        } m_ui;

        bool m_composition_not_compiled {};
        bool m_composition_not_saved {};
        bool m_composition_metadata_menu {};
        bool m_composition_mixer_menu {};
        bool m_create_instrument_menu {};
        bool m_render_composition_menu {};
        bool m_render_in_progress {};
        bool m_invalidate_ui_dock_builder {};
    };
}
