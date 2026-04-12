#include "application.hpp"

#include <algorithm>
#include <ranges>
#include <charconv>
#include <iterator>
#include <chrono>
#include <utility>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cassert>

#include <SDL3/SDL.h>
#include <imgui_internal.h>
#include <alfred/instruments.hpp>
#include <alfred/math.hpp>
#include <alfred/definitions.hpp>

#include "imgui.hpp"
#include "logging.hpp"
#include "encoder.hpp"
#include "utility.hpp"

#include "icon64.png.hpp"
#include "icon128.png.hpp"
#include "play.png.hpp"
#include "pause.png.hpp"
#include "rewind.png.hpp"

using namespace std::chrono_literals;
using namespace std::string_literals;

namespace alfred::application {
    static constexpr ImVec2 STEP_SIZE {1.0f / ui::FONT_SIZE, 18.0f / ui::FONT_SIZE};
    static constexpr float COMPOSITION_LEFT = 40.0f / ui::FONT_SIZE;
    static constexpr float COMPOSITION_HEIGHT = STEP_SIZE.y * 12.0f * float(syn::keyboard::OCTAVES) + STEP_SIZE.y * float(syn::keyboard::EXTRA);
    static constexpr float COMPOSITION_SCROLL_SPEED = 40.0f / ui::FONT_SIZE;
    static constexpr float KEYBOARD_KEY_ROUNDING = 12.0f / ui::FONT_SIZE;
    static constexpr float NOTES_ROUNDING = 6.0f / ui::FONT_SIZE;
    static constexpr float LEGATO_THICKNESS = 1.0f / ui::FONT_SIZE;
    static constexpr int ADD_MEASURES = 4;
    static constexpr unsigned long long FRAME_TIME_DEFAULT = 16;
    static constexpr unsigned long long FRAME_TIME_PLAYBACK = 3;
    static constexpr std::size_t MAX_MESSAGE_COUNT = 4;
    static constexpr std::chrono::system_clock::duration MAX_MESSAGE_DURATION = 7s;
    static constexpr const char* PRESETS_DIRECTORY = "presets";
    static constexpr double ZERO = 0.0;
    static constexpr double POINT_ONE = 0.1;
    static constexpr double ONE = 1.0;
    static constexpr double TEN = 10.0;
    static constexpr double FIFTEEN = 15.0;
    static constexpr double TWENTY = 20.0;
    static constexpr double ONE_HUNDRED = 100.0;
    static constexpr float DRAG_SPEED_FAST = 0.01f;
    static constexpr float DRAG_SPEED_SLOW = 0.001f;

    void Application::on_start() {
        desired_frame_time(FRAME_TIME_DEFAULT);

        try {
            icons({ ALFRED_ICON64, ALFRED_ICON128 });
        } catch (const video::VideoError& e) {
            logging::error("Could not set icon: {}", e.what());
        }

        try {
            utility::Buffer buffer;
            utility::read_file(utility::data_file_path() / "alfred.dat", buffer);
            data::import_data(m_data, buffer);
        } catch (const data::DataError& e) {
            m_data = {};
            logging::warning("Could not import data: {}", e.what());
            notify_message(std::format("Could not import data: {}", e.name()));
        } catch (const utility::FileError& e) {
            m_data = {};
            logging::warning("Could not import data: {}", e.what());
            notify_message(std::format("Could not import data: {}", e.name()));
        }
        
        m_synthesizer.open();
        m_synthesizer.resume();
        m_synthesizer.volume(0.9);

        // Must call this after opening the audio device
        instruments::initialize_builtin_instruments(m_synthesizer);
        reset_composition_instrument_colors();

        set_color_scheme(m_data.color_scheme);
        set_scale(m_data.scale);

        switch (m_data.scale) {
            case ui::Scale100:
            case ui::Scale125:
                break;
            case ui::Scale150:
            case ui::Scale175:
                window_size(1920, 1080);
                LOG_INFORMATION("Set a higher window size");
                break;
            case ui::Scale200:
                window_size(2560, 1440);
                LOG_INFORMATION("Set a higher window size");
                break;
        }

        auto& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.ConfigDockingNoDockingOver = true;
        io.ConfigWindowsResizeFromEdges = false;
        io.ConfigWindowsMoveFromTitleBarOnly = true;
        io.IniFilename = nullptr;

        m_instrument = instruments::SynthPiano::static_id();
        m_player = seq::Player(m_synthesizer, m_composition, [this] { stop_player(); });
        m_composition_selected_measure = m_composition.measures.end();

        m_ui.volume = m_synthesizer.volume();
        m_ui.polyphony = int(m_synthesizer.polyphony());
        m_ui.texture_play = image::Texture(m_renderer, image::Surface(ALFRED_PLAY));
        m_ui.texture_pause = image::Texture(m_renderer, image::Surface(ALFRED_PAUSE));
        m_ui.texture_rewind = image::Texture(m_renderer, image::Surface(ALFRED_REWIND));

        // Synthesizer update routine
        m_task_manager.add_repeatable_task([this] {
            m_ui.current_output_sample = m_synthesizer.update();
            m_ui.current_output_sample = std::clamp(m_ui.current_output_sample, -1.0, 1.0);
            return false;
        }, video::MAX_DELTA);

        try {
            utility::create_directory(utility::data_file_path() / PRESETS_DIRECTORY);
        } catch (const utility::FileError& e) {
            logging::error("Could not create directory: {}", e.what());
            notify_message(std::format("Could not create directory: {}", e.what()));
        }

        load_presets_from_disk();

        notify_message("Welcome! Be sure to check out the manual from the source repository.");
    }

    void Application::on_stop() {
        try {
            utility::Buffer buffer;
            data::export_data(m_data, buffer);
            utility::write_file(utility::data_file_path() / "alfred.dat", buffer);
        } catch (const data::DataError& e) {
            logging::error("Could not export data: {}", e.what());
        } catch (const utility::FileError& e) {
            logging::error("Could not export data: {}", e.what());
        }
    }

    void Application::on_update() {
        m_player.update(frame_time());
        update_messages();

        static constexpr double SMOOTHED_SPEED = 85.0;
        static constexpr double PAST_SPEED = 0.2;

        const double blend = 1.0 - std::pow(0.5, frame_time() * SMOOTHED_SPEED);
        m_ui.smoothed_output_sample = std::lerp(m_ui.smoothed_output_sample, m_ui.current_output_sample, blend);
        m_ui.past_output_sample_abs = std::max(m_ui.past_output_sample_abs, std::abs(m_ui.smoothed_output_sample));

        m_ui.past_output_sample_abs -= frame_time() * PAST_SPEED;
        m_ui.past_output_sample_abs = std::max(m_ui.past_output_sample_abs, 0.0);
    }

    void Application::on_imgui() {
        const ImGuiID dockspace_id = ImGui::GetID("Dockspace");
        const ImGuiViewport* viewport = ImGui::GetMainViewport();

        if (m_invalidate_ui_dock_builder) {
            m_invalidate_ui_dock_builder = false;
            ImGui::DockBuilderRemoveNode(dockspace_id);
        }

        if (!ImGui::DockBuilderGetNode(dockspace_id)) {
            logging::debug("Configuring the dock");

            const float width = video::DEFAULT_WIDTH * ui::scale(m_data.scale);
            const float height = video::DEFAULT_HEIGHT * ui::scale(m_data.scale);

            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace_id, ImVec2(width, height));

            ImGuiID dock_id_left {};
            ImGuiID dock_id_right = dockspace_id;

            ImGui::DockBuilderSplitNode(dock_id_right, ImGuiDir_Left, 0.18f, &dock_id_left, &dock_id_right);

            ImGuiID dock_id_left_top {};
            ImGuiID dock_id_left_bottom {};

            ImGui::DockBuilderSplitNode(dock_id_left, ImGuiDir_Up, 0.75f, &dock_id_left_top, &dock_id_left_bottom);

            ImGuiID dock_id_right_top {};
            ImGuiID dock_id_right_bottom = dock_id_right;

            ImGui::DockBuilderSplitNode(dock_id_right, ImGuiDir_Up, 0.13f, &dock_id_right_top, &dock_id_right_bottom);

            ImGuiID dock_id_right_top_left {};
            ImGuiID dock_id_right_top_right {};

            ImGui::DockBuilderSplitNode(dock_id_right_top, ImGuiDir_Left, 0.3f, &dock_id_right_top_left, &dock_id_right_top_right);

            ImGuiID dock_id_right_top2 = dock_id_right_bottom;
            ImGuiID dock_id_right_bottom2 {};

            ImGui::DockBuilderSplitNode(dock_id_right_bottom, ImGuiDir_Down, 0.3f, &dock_id_right_bottom2, &dock_id_right_top2);

            ImGui::DockBuilderDockWindow("Instruments & Synthesizer", dock_id_left_top);
            ImGui::DockBuilderDockWindow("Output", dock_id_left_bottom);
            ImGui::DockBuilderDockWindow("Playback", dock_id_right_top_left);
            ImGui::DockBuilderDockWindow("Tools", dock_id_right_top_right);
            ImGui::DockBuilderDockWindow("Keyboard", dock_id_right_bottom2);
            ImGui::DockBuilderDockWindow("Composition", dock_id_right_top2);
            ImGui::DockBuilderFinish(dockspace_id);
        }

        ImGui::DockSpaceOverViewport(dockspace_id, viewport, ImGuiDockNodeFlags_NoResize | ImGuiDockNodeFlags_NoUndocking);

        // Override default Ctrl+Tab shortcut
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Tab, ImGuiInputFlags_RouteGlobal);

        main_menu_bar();
        keyboard();
        instruments_and_synthesizer();
        output();
        playback();
        tools();
        composition();
        messages();
        composition_metadata();
        composition_mixer();
        create_instrument();
        render_composition();
        shortcuts();

#ifndef ALFRED_DISTRIBUTION
        debug();
        ImGui::ShowDemoWindow();
#endif
    }

    void Application::on_late_update() {
        m_task_manager.update();
    }

    void Application::on_event(const SDL_Event& event) {
        switch (event.type) {
            case SDL_EVENT_KEY_DOWN:
                if (keyboard_active() && !event.key.repeat) {
                    keyboard_input(event.key.key, true);
                }
                break;
            case SDL_EVENT_KEY_UP:
                keyboard_input(event.key.key, false);  // Don't block note offs
                break;
        }
    }

    void Application::main_menu_bar() {
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                main_menu_bar_file();
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Edit")) {
                main_menu_bar_edit();
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Composition")) {
                main_menu_bar_composition();
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Instrument")) {
                main_menu_bar_instrument();
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Options")) {
                main_menu_bar_options();
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Audio")) {
                main_menu_bar_audio();
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Help")) {
                main_menu_bar_help();
                ImGui::EndMenu();
            }

            ImGui::EndMainMenuBar();
        }
    }

    void Application::main_menu_bar_file() {
        if (ImGui::MenuItem("New", "Ctrl+N", false, !m_player.playing())) {
            composition_file_new();
        }

        if (ImGui::BeginMenu("Open Recent")) {
            for (const auto [i, file_path] : m_data.recent_compositions | std::views::enumerate) {
                ImGui::PushID(int(i));

                if (ImGui::MenuItem(file_path.c_str(), nullptr, false, !m_player.playing())) {
                    if (!composition_open(file_path)) {
                        m_task_manager.add_immediate_task([this, file_path] {
                            m_data.recent_compositions.erase(file_path);
                            logging::information("Erased invalid composition from the list");
                            notify_message("Erased invalid composition from the list");
                        });
                    }
                }

                ImGui::PopID();
            }

            ImGui::EndMenu();
        }

        if (ImGui::MenuItem("Open", "Ctrl+O", false, !m_player.playing())) {
            composition_file_open();
        }

        if (ImGui::MenuItem("Save", "Ctrl+S", false, !m_player.playing())) {
            composition_file_save();
        }

        if (ImGui::MenuItem("Render", "Ctrl+R", false, !m_player.playing())) {
            open_render_composition();
        }

        if (ImGui::MenuItem("Quit")) {
            m_running = false;
        }
    }

    void Application::main_menu_bar_edit() {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !m_player.playing() && !m_composition_history.undo.empty())) {
            undo();
        }

        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !m_player.playing() && !m_composition_history.redo.empty())) {
            redo();
        }

#if 0
        ImGui::Separator();

        if (ImGui::MenuItem("Cut", "Ctrl+X")) {}
        if (ImGui::MenuItem("Copy", "Ctrl+C")) {}
        if (ImGui::MenuItem("Paste", "Ctrl+V")) {}
#endif
    }

    void Application::main_menu_bar_composition() {
        if (ImGui::MenuItem("Metadata")) {
            open_composition_metadata();
        }

        if (ImGui::MenuItem("Mixer")) {
            open_composition_mixer();
        }
    }

    void Application::main_menu_bar_instrument() {
        if (ImGui::MenuItem("Create", "Ctrl+P")) {
            open_create_instrument();
        }

        if (ImGui::BeginMenu("Edit")) {
            m_synthesizer.for_each_instrument([this](const syn::Instrument& instrument) {
                if (const auto* runtime_instrument_add = dynamic_cast<const preset::add::RuntimeInstrument*>(&instrument)) {
                    if (ImGui::MenuItem(runtime_instrument_add->name())) {
                        m_ui.preset_add = translate_preset(runtime_instrument_add->preset());
                        open_create_instrument(ui::CreateInstrumentTab::Additive);
                    }
                } else if (const auto* runtime_instrument_pad = dynamic_cast<const preset::pad::RuntimeInstrument*>(&instrument)) {
                    if (ImGui::MenuItem(runtime_instrument_pad->name())) {
                        m_ui.preset_pad = translate_preset(runtime_instrument_pad->preset());
                        open_create_instrument(ui::CreateInstrumentTab::PadSynth);
                    }
                }
            });

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Load")) {
            if (ImGui::MenuItem("Additive")) {
                preset_file_open_add();
            }

            if (ImGui::MenuItem("PadSynth")) {
                preset_file_open_pad();
            }

            ImGui::EndMenu();
        }
    }

    void Application::main_menu_bar_options() {
        if (ImGui::BeginMenu("Color Scheme")) {
            constexpr const char* SCHEME[] { "Dark", "Light", "Classic" };

            if (ImGui::BeginCombo("##", SCHEME[m_data.color_scheme], ImGuiComboFlags_WidthFitPreview)) {
                for (std::size_t i {}; i < std::size(SCHEME); i++) {
                    if (ImGui::Selectable(SCHEME[i], m_data.color_scheme == i)) {
                        m_data.color_scheme = ui::ColorScheme(i);
                        set_color_scheme(m_data.color_scheme);
                    }
                }

                ImGui::EndCombo();
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Scale")) {
            constexpr const char* SCALE[] { "100%", "125%", "150%", "175%", "200%" };

            if (ImGui::BeginCombo("##", SCALE[m_data.scale], ImGuiComboFlags_WidthFitPreview)) {
                for (std::size_t i {}; i < std::size(SCALE); i++) {
                    if (ImGui::Selectable(SCALE[i], m_data.scale == i)) {
                        m_data.scale = ui::Scale(i);
                        m_task_manager.add_immediate_task([this] {
                            set_scale(m_data.scale);
                        });
                    }
                }

                ImGui::EndCombo();
            }

            ImGui::EndMenu();
        }

        if (ImGui::MenuItem("Show Keyboard", "Ctrl+K", m_data.show_keyboard)) {
            m_data.show_keyboard = !m_data.show_keyboard;
        }
    }

    void Application::main_menu_bar_audio() const {
        if (ImGui::BeginMenu("Device")) {
            ImGui::Text("%s", m_synthesizer.device().second);

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Driver")) {
            ImGui::Text("%s", m_synthesizer.driver());

            ImGui::EndMenu();
        }
    }

    void Application::main_menu_bar_help() {
        if (ImGui::BeginMenu("About")) {
            const char* link = utility::get_property(SDL_PROP_APP_METADATA_URL_STRING);
            ImGui::TextLinkOpenURL(link, link);

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Build")) {
#ifdef ALFRED_DISTRIBUTION
            constexpr const char* DEV_TAG = "";
#else
            constexpr const char* DEV_TAG = " dev";
#endif
            ImGui::Text("Version: %s%s", utility::get_property(SDL_PROP_APP_METADATA_VERSION_STRING), DEV_TAG);
#ifdef ALFRED_LINUX
            ImGui::Text("Compiler: GCC %d.%d", __GNUC__, __GNUC_MINOR__);
#elifdef ALFRED_WINDOWS
            ImGui::Text("Compiler: MSVC %d", _MSC_VER);
#endif
            ImGui::Text("Date: %s %s", __DATE__, __TIME__);

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Log File")) {
            ImGui::Text("%s", (utility::data_file_path() / logging::FILE).string().c_str());

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Presets Directory")) {
            ImGui::Text("%s", (utility::data_file_path() / PRESETS_DIRECTORY).string().c_str());

            ImGui::EndMenu();
        }
    }

    void Application::keyboard() const {
        if (!m_data.show_keyboard) {
            return;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        if (ImGui::Begin("Keyboard", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
            const Draw draw {ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), {}, {}};

            keyboard_key(draw, 'S', 1.0f, 0.0f, SDL_SCANCODE_S);
            keyboard_key(draw, 'F', 5.0f, 0.0f, SDL_SCANCODE_F);
            keyboard_key(draw, 'G', 7.0f, 0.0f, SDL_SCANCODE_G);
            keyboard_key(draw, 'J', 11.0f, 0.0f, SDL_SCANCODE_J);
            keyboard_key(draw, 'K', 13.0f, 0.0f, SDL_SCANCODE_K);
            keyboard_key(draw, 'L', 15.0f, 0.0f, SDL_SCANCODE_L);

            keyboard_key(draw, 'Z', 0.0f, 2.0f, SDL_SCANCODE_Z);
            keyboard_key(draw, 'X', 2.0f, 2.0f, SDL_SCANCODE_X);
            keyboard_key(draw, 'C', 4.0f, 2.0f, SDL_SCANCODE_C);
            keyboard_key(draw, 'V', 6.0f, 2.0f, SDL_SCANCODE_V);
            keyboard_key(draw, 'B', 8.0f, 2.0f, SDL_SCANCODE_B);
            keyboard_key(draw, 'N', 10.0f, 2.0f, SDL_SCANCODE_N);
            keyboard_key(draw, 'M', 12.0f, 2.0f, SDL_SCANCODE_M);
            keyboard_key(draw, ',', 14.0f, 2.0f, SDL_SCANCODE_COMMA);
            keyboard_key(draw, '.', 16.0f, 2.0f, SDL_SCANCODE_PERIOD);
            keyboard_key(draw, '/', 18.0f, 2.0f, SDL_SCANCODE_SLASH);
        }

        ImGui::End();

        ImGui::PopStyleVar();
    }

    void Application::keyboard_key(const Draw& draw, char key, float x, float y, int scancode) const {
        static constexpr float CELL = 30.0f / ui::FONT_SIZE;
        static constexpr float PADDING = 2.0f / ui::FONT_SIZE;
        static constexpr float TEXT_OFFSET = (2.0f * CELL - 1.0f) / 2.0f;
        static constexpr float WIDTH = 2.0f * 10.0f * CELL;
        static constexpr float HEIGHT = 2.0f * 2.0f * CELL;

        const ImColor COLOR_TEXT = color_opacity(ImGuiCol_Text, 0.8f);
        const ImColor& COLOR_INACTIVE = color(ImGuiCol_TableBorderLight);
        const ImColor& COLOR_ACTIVE = color(ImGuiCol_PlotHistogramHovered);

        const ImVec2 space = ImGui::GetContentRegionAvail();

        const ImVec2 base {(space.x - ui::rem(WIDTH)) / 2.0f, (space.y - ui::rem(HEIGHT)) / 2.0f};
        const ImVec2 position {x * ui::rem(CELL), y * ui::rem(CELL)};
        const char label[2] { key, '\0' };
        ImColor color = keyboard_state()[scancode] ? COLOR_ACTIVE : COLOR_INACTIVE;

        // Just override when keyboard is disabled
        if (!keyboard_active()) {
            color = COLOR_INACTIVE;
        }

        draw.list->AddRectFilled(
            base + draw.origin + position + ui::rem(ImVec2(PADDING, PADDING)),
            base + draw.origin + position + ImVec2(2.0f * ui::rem(CELL), 2.0f * ui::rem(CELL)) - ui::rem(ImVec2(PADDING, PADDING)),
            color,
            ui::rem(KEYBOARD_KEY_ROUNDING)
        );

        draw.list->AddText(base + draw.origin + position + ui::rem(ImVec2(TEXT_OFFSET, TEXT_OFFSET)), COLOR_TEXT, label);
    }

    void Application::instruments_and_synthesizer() {
        if (ImGui::Begin("Instruments & Synthesizer", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::SeparatorText("Active Instruments");

            if (ImGui::BeginListBox("##active_instruments")) {
                for (const auto instruments = active_instruments(); const syn::InstrumentId instrument : instruments) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ui::COLORS[m_ui.colors.at(instrument)].second);

                    if (ImGui::Selectable(m_synthesizer.get_instrument(instrument).name(), instrument == m_instrument)) {
                        m_instrument = instrument;
                        m_composition_selected_notes.clear();
                        m_synthesizer.silence();
                    }

                    ImGui::PopStyleColor();
                }

                ImGui::EndListBox();
            }

            ImGui::Dummy(ui::rem(ImVec2(0.0f, 1.0f)));

            ImGui::SeparatorText("Color");

            if (ImGui::BeginCombo("##color", ui::COLORS[m_ui.colors.at(m_instrument)].first, ImGuiComboFlags_NoArrowButton)) {
                for (std::size_t i {}; i < std::size(ui::COLORS); i++) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ui::COLORS[i].second);

                    if (ImGui::Selectable(ui::COLORS[i].first, m_ui.colors.at(m_instrument) == i)) {
                        m_ui.colors.at(m_instrument) = ui::ColorIndex(i);
                        m_composition.instrument_colors[m_instrument] = ui::ColorIndex(i);
                        modify_composition_metadata();
                    }

                    ImGui::PopStyleColor();
                }

                ImGui::EndCombo();
            }

            ImGui::Dummy(ui::rem(ImVec2(0.0f, 2.0f)));

            ImGui::SeparatorText("Instrument");

            if (ImGui::BeginCombo("##instrument", m_synthesizer.get_instrument(m_instrument).name(), ImGuiComboFlags_NoArrowButton)) {
                m_synthesizer.for_each_instrument([this](const syn::Instrument& instrument) {
                    if (ImGui::Selectable(instrument.name(), instrument.id() == m_instrument)) {
                        m_instrument = instrument.id();
                        m_composition_selected_notes.clear();
                        m_synthesizer.silence();
                    }

                    ImGui::SetItemTooltip("%s", instrument.description());
                });

                ImGui::EndCombo();
            }

            ImGui::Dummy(ui::rem(ImVec2(0.0f, 1.0f)));

            ImGui::SeparatorText("Octave");

            if (ImGui::SliderInt("##octave", &m_ui.octave, ui::OctaveFirst, ui::OctaveSeventh)) {
                m_octave = syn::keyboard::Octave(m_ui.octave);
                m_synthesizer.silence();
            }

            ImGui::SetItemTooltip("Use Q and W to change the octave");

            ImGui::Dummy(ui::rem(ImVec2(0.0f, 1.0f)));

            ImGui::SeparatorText("Polyphony");

            if (ImGui::SliderInt("##polyphony", &m_ui.polyphony, int(synthesizer::MIN_VOICES), int(synthesizer::MAX_VOICES))) {
                m_synthesizer.polyphony(std::size_t(m_ui.polyphony));
                m_synthesizer.silence_immediately();
            }
        }

        ImGui::End();
    }

    void Application::output() {
        if (ImGui::Begin("Output", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::SeparatorText("Volume");

            if (ImGui::SliderScalar("##volume", ImGuiDataType_Double, &m_ui.volume, &ZERO, &ONE, "%.2f")) {
                m_synthesizer.volume(m_ui.volume);
            }

            ImGui::SetItemTooltip("Master volume in linear scale");

            ImGui::Dummy(ui::rem(ImVec2(0.0f, 1.0f)));

            ImGui::SeparatorText("Output");

            output_indicator();
        }

        ImGui::End();
    }

    void Application::output_indicator() const {
        const ImColor COLOR = color_opacity(ImGuiCol_Text, 0.8f);

        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        const ImVec2 position = ImGui::GetCursorScreenPos();

        const auto current_sample = float(math::map(std::min(std::abs(m_ui.smoothed_output_sample), 1.0), 0.0, 1.0, 0.0, 15.0));
        const auto past_sample = float(math::map(std::min(m_ui.past_output_sample_abs, 1.0), 0.0, 1.0, 0.0, 15.0));

        const ImU32 color =
            [current_sample_abs = std::abs(m_ui.smoothed_output_sample)] {
                if (current_sample_abs > 0.9) {
                    return IM_COL32(230, 30, 30, 255);
                }

                if (current_sample_abs > 0.6) {
                    return IM_COL32(230, 230, 30, 255);
                }

                return IM_COL32(30, 230, 30, 255);
            }();

        draw_list->AddRectFilled(
            position,
            position + ui::rem(ImVec2(current_sample, 20.0f / ui::FONT_SIZE)),
            color
        );

        draw_list->AddLine(
            position + ui::rem(ImVec2(past_sample, 0.0f)),
            position + ui::rem(ImVec2(past_sample, 20.0f / ui::FONT_SIZE)),
            COLOR
        );

        draw_list->AddRect(
            position,
            position + ui::rem(ImVec2(15.0f, 20.0f / ui::FONT_SIZE)),
            COLOR
        );
    }

    void Application::playback() {
        const ImVec2 SIZE = ui::rem(ImVec2(2.461538f, 2.461538f));
        static constexpr ImVec2 UV0 {0.0f, 0.0f};
        static constexpr ImVec2 UV1 {1.0f, 1.0f};
        static constexpr ImVec4 COLOR_BACKGROUND {0.0f, 0.0f, 0.0f, 0.0f};
        const ImVec4 COLOR_FOREGROUND = color_opacity(ImGuiCol_Text, 0.8f);

        if (ImGui::Begin("Playback", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {
            if (ImGui::ImageButton("Rewind", reinterpret_cast<ImTextureID>(m_ui.texture_rewind.get()), SIZE, UV0, UV1, COLOR_BACKGROUND, COLOR_FOREGROUND)) {
                seek_player(0);
            }

            ImGui::SetItemTooltip("Rewind the player to the beginning (Backspace)");

            ImGui::SameLine();

            if (m_player.playing()) {
                if (ImGui::ImageButton("Pause", reinterpret_cast<ImTextureID>(m_ui.texture_pause.get()), SIZE, UV0, UV1, COLOR_BACKGROUND, COLOR_FOREGROUND)) {
                    stop_player();
                }
            } else {
                if (ImGui::ImageButton("Play", reinterpret_cast<ImTextureID>(m_ui.texture_play.get()), SIZE, UV0, UV1, COLOR_BACKGROUND, COLOR_FOREGROUND)) {
                    start_player();
                }
            }

            ImGui::SetItemTooltip("Play/Pause the player (Space)");

            ImGui::SameLine();

            ImGui::BeginDisabled(m_player.playing());

            if (ImGui::Checkbox("Metronome", &m_ui.metronome)) {
                if (const bool allow_edit = !m_player.playing(); allow_edit) {
                    m_player.metronome(m_ui.metronome);
                    m_composition_not_compiled = true;
                } else {
                    m_ui.metronome = !m_ui.metronome;
                }
            }

            ImGui::SetItemTooltip("Toggle the metronome on and off");

            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::Dummy(ui::rem(ImVec2(0.5f, 0.0f)));
            ImGui::SameLine();

            const Time time = elapsed_seconds_to_time(m_player.elapsed_time());

            if (m_player.in_time()) {
                ImGui::Text("%02d:%02d.%d", time.minutes, time.seconds, time.deciseconds);
            } else {
                const ImColor& COLOR = color(ImGuiCol_PlotLinesHovered);

                ImGui::TextColored(COLOR, "%02d:%02d.%d", time.minutes, time.seconds, time.deciseconds);
            }
        }

        ImGui::End();
    }

    void Application::tools() {
        if (ImGui::Begin("Tools", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::BeginGroup();

            if (ImGui::RadioButton("Measure", &m_ui.tool, ui::ToolMeasure)) {
                m_composition_selected_notes.clear();
            }

            if (ImGui::RadioButton("Note", &m_ui.tool, ui::ToolNote)) {
                m_composition_selected_measure = m_composition.measures.end();
            }

            ImGui::EndGroup();

            ImGui::SetItemTooltip("Change the editing tool (Tab)");

            ImGui::SameLine();
            ImGui::Dummy(ImVec2(ui::rem(1.0f), 0.0f));
            ImGui::SameLine();

            switch (m_ui.tool) {
                case ui::ToolMeasure:
                    tools_measure();
                    break;
                case ui::ToolNote:
                    tools_note();
                    break;
            }
        }

        ImGui::End();
    }

    void Application::tools_measure() {
        ImGui::BeginDisabled(m_player.playing());

        ImGui::BeginGroup();

        if (ImGui::Button("Append")) {
            append_measures();
        }

        ImGui::SetItemTooltip("Append a couple of measures to the end of the composition (Alt+A)");

        ImGui::BeginDisabled(m_composition_selected_measure == m_composition.measures.end());

        if (ImGui::Button("Insert")) {
            insert_measure();
        }

        ImGui::SetItemTooltip("Insert a measure after the selected measure (Alt+I)");

        ImGui::EndDisabled();

        ImGui::EndGroup();

        ImGui::SameLine();

        ImGui::BeginDisabled(m_composition_selected_measure == m_composition.measures.end());

        ImGui::BeginGroup();

        if (ImGui::Button("Clear")) {
            clear_measure();
        }

        ImGui::SetItemTooltip("Clear the selected measure (Alt+C)");

        if (ImGui::Button("Delete")) {
            delete_measure();
        }

        ImGui::SetItemTooltip("Completely delete the selected measure (Alt+D)");

        ImGui::EndGroup();

        ImGui::SameLine();
        ImGui::Dummy(ui::rem(ImVec2(0.5f, 0.0f)));
        ImGui::SameLine();

        if (time_signature()) {
            set_measure_time_signature();
        }

        ImGui::SameLine();
        ImGui::Dummy(ui::rem(ImVec2(0.5f, 0.0f)));
        ImGui::SameLine();

        if (dynamics()) {
            set_measure_dynamics();
        }

        ImGui::SameLine();
        ImGui::Dummy(ui::rem(ImVec2(0.5f, 0.0f)));
        ImGui::SameLine();

        if (agogic()) {
            set_measure_agogic();
        }

        ImGui::EndDisabled();

        ImGui::EndDisabled();
    }

    void Application::tools_note() {
        ImGui::BeginDisabled(m_player.playing() || m_composition_selected_notes.empty());

        ImGui::BeginGroup();

        ImGui::BeginGroup();

        ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);

        if (ImGui::ArrowButton("Shift Up", ImGuiDir_Up)) {
            shift_notes_up();
        }

        ImGui::SetItemTooltip("Shift the selected notes up (Up)");

        ImGui::SameLine();

        if (ImGui::ArrowButton("Shift Down", ImGuiDir_Down)) {
            shift_notes_down();
        }

        ImGui::SetItemTooltip("Shift the selected notes down (Down)");

        if (ImGui::ArrowButton("Shift Left", ImGuiDir_Left)) {
            shift_notes_left();
        }

        ImGui::SetItemTooltip("Shift the selected notes left (Left)");

        ImGui::SameLine();

        if (ImGui::ArrowButton("Shift Right", ImGuiDir_Right)) {
            shift_notes_right();
        }

        ImGui::SetItemTooltip("Shift the selected notes right (Right)");

        ImGui::PopItemFlag();

        ImGui::EndGroup();

        ImGui::SameLine();

        ImGui::BeginGroup();

        if (ImGui::Button("Delete")) {
            delete_notes();
        }

        ImGui::SetItemTooltip("Completely delete the selected notes (Alt+D)");

        if (ImGui::Button("Legato")) {
            legato_notes();
        }

        ImGui::SetItemTooltip("Toggle the selected notes' legato (Alt+L)");

        ImGui::EndGroup();

        ImGui::SameLine();

        ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);

        if (ImGui::ArrowButton("Remove Delay", ImGuiDir_Left)) {
            remove_delay_notes();
        }

        ImGui::SetItemTooltip("Remove delay from the selected notes (Alt+,)");

        ImGui::SameLine();

        if (ImGui::ArrowButton("Add Delay", ImGuiDir_Right)) {
            add_delay_notes();
        }

        ImGui::SetItemTooltip("Add delay to the selected notes (Alt+.)");

        ImGui::PopItemFlag();

        ImGui::EndGroup();

        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::Dummy(ui::rem(ImVec2(0.5f, 0.0f)));
        ImGui::SameLine();

        ImGui::BeginGroup();

        ImGui::RadioButton("Whole", &m_ui.value, ui::ValueWhole);

        ImGui::SameLine();

        ImGui::RadioButton("Half", &m_ui.value, ui::ValueHalf);

        ImGui::SameLine();

        ImGui::RadioButton("Quarter", &m_ui.value, ui::ValueQuarter);

        ImGui::RadioButton("Eighth", &m_ui.value, ui::ValueEighth);

        ImGui::SameLine();

        ImGui::RadioButton("Sixteenth", &m_ui.value, ui::ValueSixteenth);

        ImGui::EndGroup();

        ImGui::SetItemTooltip("Change the note value (1-5)");

        ImGui::SameLine();
        ImGui::Dummy(ui::rem(ImVec2(0.5f, 0.0f)));
        ImGui::SameLine();

        ImGui::BeginGroup();

        ImGui::RadioButton("None", &m_ui.tuplet, ui::TupletNone);

        ImGui::SameLine();

        ImGui::RadioButton("Triplet", &m_ui.tuplet, ui::TupletTriplet);

        ImGui::EndGroup();

        ImGui::SetItemTooltip("Change the note tuplet type");
    }

    void Application::composition() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        if (ImGui::Begin("Composition", nullptr, ImGuiWindowFlags_NoResize)) {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const ImVec2 space = ImGui::GetContentRegionAvail();
            const ImVec2 clamped_space = composition_space(space);
            const Draw draw {ImGui::GetWindowDrawList(), origin, space, clamped_space};

            (void) ImGui::InvisibleButton("Canvas", space, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);

            const bool item_active = ImGui::IsItemActive();
            const bool item_hovered = ImGui::IsItemHovered();
            const bool allow_edit = !m_player.playing();

            std::optional<HoveredNote> hovered_note;

            if (!ImGui::IsKeyDown(ImGuiMod_Alt)) {
                if (hovered_note = hover_note(composition_mouse_position(origin)); item_hovered && allow_edit && hovered_note) {
                    composition_hover(draw, *hovered_note);
                }
            }

            composition_camera(item_active, item_hovered, space);
            composition_measures(draw);
            composition_octaves(draw);
            composition_cursor(draw);
            composition_notes(draw);
            composition_measures_labels(draw);
            composition_left(draw);

            if (item_hovered && allow_edit && hovered_note) {
                composition_pitch(draw, *hovered_note);
            }

            if (item_hovered && allow_edit) {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    composition_mouse_pressed(origin);
                }
            }
        }

        ImGui::End();

        ImGui::PopStyleVar();
    }

    void Application::composition_left(const Draw& draw) const {
        static constexpr ImVec2 CELL {COMPOSITION_LEFT, STEP_SIZE.y};
        static constexpr ImVec2 TEXT_OFFSET {(CELL.x - 2.0f) / 2.0f, (CELL.y - 1.0f) / 2.0f};

        const ImColor COLOR_FOREGROUND = color_opacity(ImGuiCol_Text, 0.8f);
        const ImColor COLOR_BACKGROUND = color_opacity(ImGuiCol_WindowBg, 1.0f);

        draw.list->AddRectFilled(
            draw.origin + ImVec2(0.0f, 0.0f),
            draw.origin + ImVec2(ui::rem(COMPOSITION_LEFT), draw.clamped_space.y),
            COLOR_BACKGROUND
        );

        draw.list->AddLine(
            draw.origin + ImVec2(ui::rem(COMPOSITION_LEFT), 0.0f),
            draw.origin + ImVec2(ui::rem(COMPOSITION_LEFT), draw.clamped_space.y),
            COLOR_FOREGROUND
        );

        float position_y {};

        for (int id = syn::keyboard::ID_END; id >= int(syn::keyboard::ID_BEGIN); id--) {
            if (point_y_in_camera_view(position_y + ui::rem(STEP_SIZE.y), draw.space.y + ui::rem(STEP_SIZE.y))) {
                char note_pitch[4] {};
                note_to_string(syn::NoteId(id), note_pitch);

                draw.list->AddText(
                    draw.origin + ImVec2(0.0f, position_y) + ui::rem(TEXT_OFFSET) - ImVec2(0.0f, m_composition_camera.y),
                    COLOR_FOREGROUND,
                    note_pitch
                );

                draw.list->AddLine(
                    draw.origin + ImVec2(0.0f, position_y + ui::rem(STEP_SIZE.y)) - ImVec2(0.0f, m_composition_camera.y),
                    draw.origin + ImVec2(ui::rem(COMPOSITION_LEFT), position_y + ui::rem(STEP_SIZE.y)) - ImVec2(0.0f, m_composition_camera.y),
                    COLOR_FOREGROUND
                );
            }

            position_y += ui::rem(STEP_SIZE.y);
        }
    }

    void Application::composition_octaves(const Draw& draw) const {
        const ImColor COLOR = color_opacity(ImGuiCol_TextDisabled, 0.7f);

        float position_y = float(syn::keyboard::EXTRA) * ui::rem(STEP_SIZE.y);

        if (point_y_in_camera_view(position_y, draw.space.y)) {
            draw.list->AddLine(
                draw.origin + ImVec2(ui::rem(COMPOSITION_LEFT), position_y) - ImVec2(0.0f, m_composition_camera.y),
                draw.origin + ImVec2(ui::rem(COMPOSITION_LEFT) + draw.clamped_space.x, position_y) - ImVec2(0.0f, m_composition_camera.y),
                COLOR
            );
        }

        // Draw one additional line
        for (int i = 1; i <= syn::keyboard::OCTAVES; i++) {
            position_y += 12.0f * ui::rem(STEP_SIZE.y);

            if (!point_y_in_camera_view(position_y, draw.space.y)) {
                continue;
            }

            draw.list->AddLine(
                draw.origin + ImVec2(ui::rem(COMPOSITION_LEFT), position_y) - ImVec2(0.0f, m_composition_camera.y),
                draw.origin + ImVec2(ui::rem(COMPOSITION_LEFT) + draw.clamped_space.x, position_y) - ImVec2(0.0f, m_composition_camera.y),
                COLOR
            );
        }
    }

    void Application::composition_measures(const Draw& draw) const {
        const ImColor COLOR_FOREGROUND = color_opacity(ImGuiCol_Text, 0.8f);
        const ImColor COLOR_FOREGROUND2 = color_opacity(ImGuiCol_Text, 0.2f);
        const ImColor& COLOR_SELECTION = color(ImGuiCol_FrameBg);

        float position_x = ui::rem(COMPOSITION_LEFT);

        for (auto measure = m_composition.measures.begin(); measure != m_composition.measures.end(); measure++) {
            const float width = float(measure->time_signature.measure_steps()) * ui::rem(STEP_SIZE.x);

            if (measure == m_composition_selected_measure) {
                draw.list->AddRectFilled(
                    draw.origin + ImVec2(position_x + 1.0f, 0.0f) - ImVec2(m_composition_camera.x, 0.0f),
                    draw.origin + ImVec2(position_x + width, draw.clamped_space.y) - ImVec2(m_composition_camera.x, 0.0f),
                    COLOR_SELECTION
                );
            }

            const bool in_view = point_x_in_camera_view(position_x + width, draw.space.x + width);

            if (in_view) {
                for (seq::Beats beat = 1; beat < measure->time_signature.beats(); beat++) {
                    const float position_x_beat =
                        position_x +
                        float(beat) *
                        float(seq::steps(measure->time_signature.value())) * ui::rem(STEP_SIZE.x);

                    draw.list->AddLine(
                        draw.origin + ImVec2(position_x_beat, 0.0f) - ImVec2(m_composition_camera.x, 0.0f),
                        draw.origin + ImVec2(position_x_beat, draw.clamped_space.y) - ImVec2(m_composition_camera.x, 0.0f),
                        COLOR_FOREGROUND2
                    );
                }
            }

            position_x += width;

            if (in_view) {
                draw.list->AddLine(
                    draw.origin + ImVec2(position_x, 0.0f) - ImVec2(m_composition_camera.x, 0.0f),
                    draw.origin + ImVec2(position_x, draw.clamped_space.y) - ImVec2(m_composition_camera.x, 0.0f),
                    COLOR_FOREGROUND
                );
            }
        }
    }

    void Application::composition_measures_labels(const Draw& draw) const {
        static constexpr ImVec2 TEXT_OFFSET {5.0f / ui::FONT_SIZE, 5.0f / ui::FONT_SIZE};

        const ImColor COLOR = color_opacity(ImGuiCol_Text, 0.8f);

        float position_x = ui::rem(COMPOSITION_LEFT);

        for (const auto [i, measure] : m_composition.measures | std::views::enumerate) {
            const float width = float(measure.time_signature.measure_steps()) * ui::rem(STEP_SIZE.x);

            if (point_x_in_camera_view(position_x + width, draw.space.x + width)) {
                char buffer[32] {};

                draw.list->AddText(
                    draw.origin + ImVec2(position_x, 0.0f) + ui::rem(TEXT_OFFSET) - ImVec2(m_composition_camera.x, 0.0f),
                    COLOR,
                    measure_label(buffer, long(i + 1))
                );
            }

            position_x += width;
        }
    }

    void Application::composition_notes(const Draw& draw) const {
        const ImColor& COLOR = color(ImGuiCol_Text);

        float global_position_x = ui::rem(COMPOSITION_LEFT);

        for (auto measure = m_composition.measures.begin(); measure != m_composition.measures.end(); measure++) {
            const float width = float(measure->time_signature.measure_steps()) * ui::rem(STEP_SIZE.x);

            if (point_x_in_camera_view(global_position_x + width, draw.space.x + width)) {
                for (const auto& [instrument, notes] : measure->instruments) {
                    if (instrument == m_instrument) {
                        continue;
                    }

                    composition_notes(draw, instrument, notes, global_position_x);
                }

                // Always draw the selected instrument last
                if (auto instrument = measure->instruments.find(m_instrument); instrument != measure->instruments.end()) {
                    composition_notes(draw, instrument->first, instrument->second, global_position_x);
                }

                for (const ProvenanceNote& provenance_note : m_composition_selected_notes) {
                    if (provenance_note.measure() == measure) {
                        const ImVec4 rect = note_rectangle(*provenance_note.note());

                        draw.list->AddRect(
                            draw.origin + ImVec2(global_position_x + rect.x, rect.y) - m_composition_camera,
                            draw.origin + ImVec2(global_position_x + rect.x + rect.z, rect.y + rect.w) - m_composition_camera,
                            COLOR,
                            ui::rem(NOTES_ROUNDING)
                        );
                    }
                }
            }

            global_position_x += width;
        }
    }

    void Application::composition_notes(const Draw& draw, syn::InstrumentId instrument, const seq::Notes& notes, float global_position_x) const {
        for (auto note = notes.begin(); note != notes.end(); note++) {
            const ImVec4 rect = note_rectangle(*note);

            draw.list->AddRectFilled(
                draw.origin + ImVec2(global_position_x + rect.x, rect.y) - m_composition_camera,
                draw.origin + ImVec2(global_position_x + rect.x + rect.z, rect.y + rect.w) - m_composition_camera,
                ui::COLORS[m_ui.colors.at(instrument)].second,
                ui::rem(NOTES_ROUNDING)
            );

            if (note->legato) {
                const float x = global_position_x + float(note->position + seq::steps(note->value, note->tuplet)) * ui::rem(STEP_SIZE.x);

                draw.list->AddBezierCubic(
                    draw.origin + ImVec2(x - ui::rem(STEP_SIZE.x) * 6.0f, rect.y + ui::rem(STEP_SIZE.y) - 4.0f) - m_composition_camera,
                    draw.origin + ImVec2(x - ui::rem(STEP_SIZE.x) * 3.0f, rect.y + ui::rem(STEP_SIZE.y) * 1.5f - 4.0f) - m_composition_camera,
                    draw.origin + ImVec2(x + ui::rem(STEP_SIZE.x) * 3.0f, rect.y + ui::rem(STEP_SIZE.y) * 1.5f - 4.0f) - m_composition_camera,
                    draw.origin + ImVec2(x + ui::rem(STEP_SIZE.x) * 6.0f, rect.y + ui::rem(STEP_SIZE.y) - 4.0f) - m_composition_camera,
                    ui::COLORS[m_ui.colors.at(instrument)].second,
                    ui::rem(LEGATO_THICKNESS)
                );
            }
        }
    }

    void Application::composition_cursor(const Draw& draw) const {
        const ImColor& COLOR = color(ImGuiCol_PlotHistogramHovered);

        const float position_x = ui::rem(COMPOSITION_LEFT) + float(m_player.position()) * ui::rem(STEP_SIZE.x);

        draw.list->AddLine(
            draw.origin + ImVec2(position_x, 0.0f) - ImVec2(m_composition_camera.x, 0.0f),
            draw.origin + ImVec2(position_x, draw.clamped_space.y) - ImVec2(m_composition_camera.x, 0.0f),
            COLOR
        );
    }

    void Application::composition_hover(const Draw& draw, const HoveredNote& hovered_note) const {
        const ImColor COLOR = color_opacity(ImGuiCol_FrameBg, 0.3f);
        const ImColor& COLOR2 = color(ImGuiCol_FrameBg);

        switch (m_ui.tool) {
            case ui::ToolMeasure: {
                const float position_x = ui::rem(COMPOSITION_LEFT) + float(hovered_note.measure_position()) * ui::rem(STEP_SIZE.x);
                const float width = float(hovered_note.measure()->time_signature.measure_steps()) * ui::rem(STEP_SIZE.x);

                draw.list->AddRectFilled(
                    draw.origin + ImVec2(position_x + 1.0f, 0.0f) - ImVec2(m_composition_camera.x, 0.0f),
                    draw.origin + ImVec2(position_x + width, draw.clamped_space.y) - ImVec2(m_composition_camera.x, 0.0f),
                    COLOR
                );

                break;
            }
            case ui::ToolNote: {
                const float position_y = float(syn::keyboard::NOTES - 1 - hovered_note.id()) * ui::rem(STEP_SIZE.y);

                draw.list->AddRectFilled(
                    draw.origin + ImVec2(ui::rem(COMPOSITION_LEFT), position_y) - ImVec2(0.0f, m_composition_camera.y),
                    draw.origin + ImVec2(ui::rem(COMPOSITION_LEFT) + draw.clamped_space.x, position_y + ui::rem(STEP_SIZE.y)) - ImVec2(0.0f, m_composition_camera.y),
                    COLOR
                );

                const float position_x = ui::rem(COMPOSITION_LEFT) + float(hovered_note.global_position() / seq::DIVISION * seq::DIVISION) * ui::rem(STEP_SIZE.x);

                draw.list->AddRectFilled(
                    draw.origin + ImVec2(position_x, position_y) - m_composition_camera,
                    draw.origin + ImVec2(position_x + ui::rem(STEP_SIZE.x) * float(seq::DIVISION), position_y + ui::rem(STEP_SIZE.y)) - m_composition_camera,
                    COLOR2
                );

                break;
            }
        }
    }

    void Application::composition_pitch(const Draw& draw, const HoveredNote& hovered_note) const {
        const auto [begin, end] = m_synthesizer.get_instrument(m_instrument).range();
        const ImColor& COLOR = hovered_note.id() >= begin && hovered_note.id() <= end ? color(ImGuiCol_Text) : color(ImGuiCol_PlotLinesHovered);

        char note_pitch[4] {};
        note_to_string(hovered_note.id(), note_pitch);

        draw.list->AddText(
            ImGui::GetMousePos() + ImVec2(ui::rem(1.0f), -ui::rem(1.5f)),
            COLOR,
            note_pitch
        );
    }

    void Application::shortcuts() {
        if (!m_player.playing()) {
            if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, ImGuiInputFlags_RouteAlways)) {
                composition_file_new();
            }

            if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_RouteAlways)) {
                composition_file_open();
            }

            if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteAlways)) {
                composition_file_save();
            }

            if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteAlways)) {
                if (!m_composition_history.undo.empty()) {
                    undo();
                }
            }

            if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteAlways)) {
                if (!m_composition_history.redo.empty()) {
                    redo();
                }
            }

            if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_R, ImGuiInputFlags_RouteAlways)) {
                open_render_composition();
            }
        }

        if (ImGui::Shortcut(ImGuiKey_Space, ImGuiInputFlags_RouteGlobal)) {
            if (m_player.playing()) {
                stop_player();
            } else {
                start_player();
            }
        }

        if (ImGui::Shortcut(ImGuiKey_Backspace, ImGuiInputFlags_RouteGlobal)) {
            seek_player(0);
        }

        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, ImGuiInputFlags_RouteAlways)) {
            open_create_instrument();
        }

        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_K, ImGuiInputFlags_RouteAlways)) {
            m_data.show_keyboard = !m_data.show_keyboard;
        }

        if (ImGui::Shortcut(ImGuiKey_Q, ImGuiInputFlags_RouteGlobal)) {
            m_ui.octave = std::max(int(syn::keyboard::OCTAVE_BEGIN), m_ui.octave - 1);

            if (m_octave > syn::keyboard::OCTAVE_BEGIN) {
                m_octave = syn::keyboard::Octave(std::uint32_t(m_octave) - 1);
                m_synthesizer.silence();
            }
        }

        if (ImGui::Shortcut(ImGuiKey_W, ImGuiInputFlags_RouteGlobal)) {
            m_ui.octave = std::min(int(syn::keyboard::OCTAVE_END), m_ui.octave + 1);

            if (m_octave < syn::keyboard::OCTAVE_END) {
                m_octave = syn::keyboard::Octave(std::uint32_t(m_octave) + 1);
                m_synthesizer.silence();
            }
        }

        if (ImGui::Shortcut(ImGuiKey_Tab, ImGuiInputFlags_RouteGlobal)) {
            switch (m_ui.tool) {
                case ui::ToolMeasure:
                    m_ui.tool = ui::ToolNote;
                    m_composition_selected_measure = m_composition.measures.end();
                    break;
                case ui::ToolNote:
                    m_ui.tool = ui::ToolMeasure;
                    m_composition_selected_notes.clear();
                    break;
            }
        }

        switch (m_ui.tool) {
            case ui::ToolMeasure:
                if (m_player.playing()) {
                    break;
                }

                if (ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_A, ImGuiInputFlags_RouteAlways | ImGuiInputFlags_Repeat)) {
                    append_measures();
                }

                if (m_composition_selected_measure == m_composition.measures.end()) {
                    break;
                }

                if (ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_I, ImGuiInputFlags_RouteAlways | ImGuiInputFlags_Repeat)) {
                    insert_measure();
                }

                if (ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_C, ImGuiInputFlags_RouteAlways)) {
                    clear_measure();
                }

                if (ImGui::Shortcut(ImGuiKey_Delete, ImGuiInputFlags_RouteAlways)) {
                    delete_measure();
                }

                if (ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_D, ImGuiInputFlags_RouteAlways)) {
                    delete_measure();
                }

                break;
            case ui::ToolNote:
                if (ImGui::Shortcut(ImGuiKey_1, ImGuiInputFlags_RouteAlways)) {
                    m_ui.value = ui::ValueWhole;
                }

                if (ImGui::Shortcut(ImGuiKey_2, ImGuiInputFlags_RouteAlways)) {
                    m_ui.value = ui::ValueHalf;
                }

                if (ImGui::Shortcut(ImGuiKey_3, ImGuiInputFlags_RouteAlways)) {
                    m_ui.value = ui::ValueQuarter;
                }

                if (ImGui::Shortcut(ImGuiKey_4, ImGuiInputFlags_RouteAlways)) {
                    m_ui.value = ui::ValueEighth;
                }

                if (ImGui::Shortcut(ImGuiKey_5, ImGuiInputFlags_RouteAlways)) {
                    m_ui.value = ui::ValueSixteenth;
                }

                if (m_player.playing() || m_composition_selected_notes.empty()) {
                    break;
                }

                if (ImGui::Shortcut(ImGuiKey_UpArrow, ImGuiInputFlags_RouteAlways | ImGuiInputFlags_Repeat)) {
                    shift_notes_up();
                }

                if (ImGui::Shortcut(ImGuiKey_DownArrow, ImGuiInputFlags_RouteAlways | ImGuiInputFlags_Repeat)) {
                    shift_notes_down();
                }

                if (ImGui::Shortcut(ImGuiKey_LeftArrow, ImGuiInputFlags_RouteAlways | ImGuiInputFlags_Repeat)) {
                    shift_notes_left();
                }

                if (ImGui::Shortcut(ImGuiKey_RightArrow, ImGuiInputFlags_RouteAlways | ImGuiInputFlags_Repeat)) {
                    shift_notes_right();
                }

                if (ImGui::Shortcut(ImGuiKey_Delete, ImGuiInputFlags_RouteAlways)) {
                    delete_notes();
                }

                if (ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_D, ImGuiInputFlags_RouteAlways)) {
                    delete_notes();
                }

                if (ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_L, ImGuiInputFlags_RouteAlways)) {
                    legato_notes();
                }

                if (ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_Comma, ImGuiInputFlags_RouteAlways | ImGuiInputFlags_Repeat)) {
                    remove_delay_notes();
                }

                if (ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_Period, ImGuiInputFlags_RouteAlways | ImGuiInputFlags_Repeat)) {
                    add_delay_notes();
                }

                break;
        }

        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiMod_Alt | ImGuiKey_F, ImGuiInputFlags_RouteAlways)) {
            m_synthesizer.store_instrument(std::make_unique<instruments::EasterEgg>());
            set_composition_instrument_colors();
            notify_message("Wow, wow! Hold on there. Are you really sure you want to do that?");
        }
    }

    bool Application::time_signature() {
        constexpr const char* BEATS[] { "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16" };
        constexpr const char* VALUE[] { "1", "2", "4", "8", "16" };

        static constexpr auto flags = ImGuiComboFlags_HeightSmall | ImGuiComboFlags_WidthFitPreview | ImGuiComboFlags_NoArrowButton;

        bool result {};

        ImGui::BeginGroup();

        if (ImGui::BeginCombo("Beats", BEATS[m_ui.time_signature.beats], flags)) {
            for (std::size_t i{}; i < std::size(BEATS); i++) {
                if (ImGui::Selectable(BEATS[i], m_ui.time_signature.beats == i)) {
                    m_ui.time_signature.beats = ui::TimeSignature::Beats(i);
                    result = true;
                }
            }

            ImGui::EndCombo();
        }

        if (ImGui::BeginCombo("Value", VALUE[m_ui.time_signature.value], flags)) {
            for (std::size_t i{}; i < std::size(VALUE); i++) {
                if (ImGui::Selectable(VALUE[i], m_ui.time_signature.value == i)) {
                    m_ui.time_signature.value = ui::TimeSignature::Value(i);
                    result = true;
                }
            }

            ImGui::EndCombo();
        }

        ImGui::EndGroup();

        ImGui::SetItemTooltip("Change the time signature of the selected measure");

        return result;
    }

    bool Application::dynamics() {
        bool result {};

        ImGui::BeginGroup();

        if (ImGui::Button(m_ui.dynamics.varying ? "Varying##dynamics" : "Constant##dynamics")) {
            m_ui.dynamics.varying = !m_ui.dynamics.varying;
            result = true;
        }

        static constexpr auto flags = ImGuiComboFlags_HeightSmall | ImGuiComboFlags_WidthFitPreview | ImGuiComboFlags_NoArrowButton;
        constexpr const char* LOUDNESS[] { "ppp", "pp", "p", "mp", "mf", "f", "ff", "fff" };

        if (m_ui.dynamics.varying) {
            if (ImGui::BeginCombo("##loudness1", LOUDNESS[m_ui.dynamics.loudness1], flags)) {
                for (std::size_t i {}; i < std::size(LOUDNESS); i++) {
                    if (ImGui::Selectable(LOUDNESS[i], m_ui.dynamics.loudness1 == int(i))) {
                        m_ui.dynamics.loudness1 = ui::Loudness(i);
                        result = true;
                    }
                }

                ImGui::EndCombo();
            }

            ImGui::SameLine();

            if (ImGui::BeginCombo("##loudness2", LOUDNESS[m_ui.dynamics.loudness2], flags)) {
                for (std::size_t i {}; i < std::size(LOUDNESS); i++) {
                    if (ImGui::Selectable(LOUDNESS[i], m_ui.dynamics.loudness2 == int(i))) {
                        m_ui.dynamics.loudness2 = ui::Loudness(i);
                        result = true;
                    }
                }

                ImGui::EndCombo();
            }
        } else {
            if (ImGui::BeginCombo("##loudness", LOUDNESS[m_ui.dynamics.loudness1], flags)) {
                for (std::size_t i {}; i < std::size(LOUDNESS); i++) {
                    if (ImGui::Selectable(LOUDNESS[i], m_ui.dynamics.loudness1 == int(i))) {
                        m_ui.dynamics.loudness1 = ui::Loudness(i);
                        result = true;
                    }
                }

                ImGui::EndCombo();
            }
        }

        ImGui::EndGroup();

        ImGui::SetItemTooltip("Change the dynamics of the selected measure");

        return result;
    }

    bool Application::agogic() {
        constexpr std::uint32_t one = 1;

        bool result {};

        ImGui::BeginGroup();

        if (ImGui::Button(m_ui.agogic.varying ? "Varying##agogic" : "Constant##agogic")) {
            m_ui.agogic.varying = !m_ui.agogic.varying;
            result = true;
        }

        if (m_ui.agogic.varying) {
            ImGui::SetNextItemWidth(ui::rem(6.0f));

            if (ImGui::InputScalar("##tempo1", ImGuiDataType_U32, &m_ui.agogic.tempo1, &one)) {
                m_ui.agogic.tempo1 = std::clamp(m_ui.agogic.tempo1, seq::Tempo::MIN, seq::Tempo::MAX);
                result = true;
            }

            ImGui::SameLine();

            ImGui::SetNextItemWidth(ui::rem(6.0f));

            if (ImGui::InputScalar("##tempo2", ImGuiDataType_U32, &m_ui.agogic.tempo2, &one)) {
                m_ui.agogic.tempo2 = std::clamp(m_ui.agogic.tempo2, seq::Tempo::MIN, seq::Tempo::MAX);
                result = true;
            }
        } else {
            ImGui::SetNextItemWidth(ui::rem(6.0f));

            if (ImGui::InputScalar("##tempo", ImGuiDataType_U32, &m_ui.agogic.tempo1, &one)) {
                m_ui.agogic.tempo1 = std::clamp(m_ui.agogic.tempo1, seq::Tempo::MIN, seq::Tempo::MAX);
                result = true;
            }
        }

        ImGui::EndGroup();

        ImGui::SetItemTooltip("Change the agogic of the selected measure in quarters per minute");

        return result;
    }

    void Application::composition_metadata() {
        window_menu("Composition Metadata", m_composition_metadata_menu, [this] {
            if (ImGui::InputText("Title", m_ui.composition.title, sizeof(m_ui.composition.title))) {
                m_composition.title = m_ui.composition.title;
                modify_composition_metadata();
            }

            if (ImGui::InputText("Author", m_ui.composition.author, sizeof(m_ui.composition.author))) {
                m_composition.author = m_ui.composition.author;
                modify_composition_metadata();
            }

            if (ImGui::InputScalar("Year", ImGuiDataType_S16, &m_ui.composition.year)) {
                m_composition.year = std::chrono::year(m_ui.composition.year);
                modify_composition_metadata();
            }
        });
    }

    void Application::composition_mixer() {
        window_menu("Composition Mixer", m_composition_mixer_menu, [this] {
            const auto instruments = active_instruments();

            if (instruments.empty()) {
                ImGui::Text("No Instruments");
            }

            for (const syn::InstrumentId instrument : instruments) {
                const char* name = m_synthesizer.get_instrument(instrument).name();

                ImGui::PushID(name);

                ImGui::TextColored(ImColor(ui::COLORS[m_ui.colors.at(instrument)].second), "%s", name);

                ImGui::SameLine();

                int volume = std::clamp(m_composition.instrument_volumes[instrument], syn::volume::MIN, syn::volume::MAX);

                if (ImGui::SliderInt("##volume", &volume, syn::volume::MIN, syn::volume::MAX, "%d dB")) {
                    m_composition.instrument_volumes[instrument] = volume;
                    modify_composition_metadata();
                }

                ImGui::Dummy(ui::rem(ImVec2(0.0f, 0.5f)));

                ImGui::PopID();
            }
        });
    }

    void Application::create_instrument() {
        window_menu("Create Instrument", m_create_instrument_menu, [this] {
            if (ImGui::BeginTabBar("Create Instrument")) {
                auto flags = m_ui.create_instrument_tab_select && *m_ui.create_instrument_tab_select == ui::CreateInstrumentTab::Additive
                    ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;

                if (ImGui::BeginTabItem("Additive", nullptr, flags)) {
                    create_instrument_base(m_ui.preset_add);
                    create_instrument_add();
                    m_ui.create_instrument_tab = ui::CreateInstrumentTab::Additive;

                    ImGui::EndTabItem();
                }

                flags = m_ui.create_instrument_tab_select && *m_ui.create_instrument_tab_select == ui::CreateInstrumentTab::PadSynth
                    ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;

                if (ImGui::BeginTabItem("PadSynth", nullptr, flags)) {
                    create_instrument_base(m_ui.preset_pad);
                    create_instrument_pad();
                    m_ui.create_instrument_tab = ui::CreateInstrumentTab::PadSynth;

                    ImGui::EndTabItem();
                }

                m_ui.create_instrument_tab_select = std::nullopt;

                ImGui::EndTabBar();
            }

            switch (m_ui.create_instrument_tab) {
                case ui::CreateInstrumentTab::Additive:
                    create_instrument_buttons_add();
                    break;
                case ui::CreateInstrumentTab::PadSynth:
                    create_instrument_buttons_pad();
                    break;
            }
        });
    }

    void Application::create_instrument_envelope(ui::preset::Envelope& envelope) {
        if (envelope.type != ui::preset::EnvelopeTypeNull) {
            ImGui::DragScalar("Attack", ImGuiDataType_Double, &envelope.description.duration_attack, DRAG_SPEED_FAST, &ZERO, &TEN, "%.3f", ImGuiSliderFlags_ClampOnInput);
            ImGui::DragScalar("Decay", ImGuiDataType_Double, &envelope.description.duration_decay, DRAG_SPEED_FAST, &ZERO, &TEN, "%.3f", ImGuiSliderFlags_ClampOnInput);
            ImGui::DragScalar("Release", ImGuiDataType_Double, &envelope.description.duration_release, DRAG_SPEED_FAST, &ZERO, &TEN, "%.3f", ImGuiSliderFlags_ClampOnInput);
        }

        switch (envelope.type) {
            case ui::preset::EnvelopeTypeAdsrLinear:
            case ui::preset::EnvelopeTypeAdsrExponential:
                ImGui::DragScalar("Sustain", ImGuiDataType_Double, &envelope.description.value_sustain, DRAG_SPEED_SLOW, &ZERO, &ONE, "%.3f", ImGuiSliderFlags_ClampOnInput);
                break;
            case ui::preset::EnvelopeTypeAdrLinear:
            case ui::preset::EnvelopeTypeAdrExponential:
                break;
            case ui::preset::EnvelopeTypeNull:
                break;
        }

        constexpr const char* ENVELOPE_TYPE[] { "ADSR Linear", "ADSR Exponential", "ADR Linear", "ADR Exponential", "Null" };

        if (ImGui::BeginCombo("Envelope Type", ENVELOPE_TYPE[envelope.type])) {
            for (std::size_t i {}; i < std::size(ENVELOPE_TYPE); i++) {
                if (ImGui::Selectable(ENVELOPE_TYPE[i], envelope.type == int(i))) {
                    envelope.type = ui::preset::EnvelopeType(i);
                }
            }

            ImGui::EndCombo();
        }
    }

    void Application::create_instrument_base(ui::preset::BasePreset& preset) {
        ImGui::InputText("Name", preset.name, sizeof(preset.name));

        ImGui::SetItemTooltip("Has to be unique across all instruments in a synthesizer's storage");

        ImGui::InputText("Description", preset.description, sizeof(preset.description));

        if (ImGui::InputScalarN("Range", ImGuiDataType_U32, preset.range, 2)) {
            preset.range[0] = std::clamp(preset.range[0], syn::keyboard::ID_BEGIN, syn::keyboard::ID_END);
            preset.range[1] = std::clamp(preset.range[1], syn::keyboard::ID_BEGIN, syn::keyboard::ID_END);
            preset.range[0] = std::min(preset.range[0], preset.range[1]);
        }

        create_instrument_envelope(preset.envelope);
    }

    void Application::create_instrument_add() {
        ImGui::Dummy(ui::rem(ImVec2(0.0f, 0.5f)));

        if (ImGui::Button("Add Partial")) {
            auto& partial = m_ui.preset_add.partials.emplace_back();
            partial.envelope.type = ui::preset::EnvelopeTypeNull;
        }

        if (ImGui::BeginChild("Partials", ImVec2(0.0f, ui::rem(22.0f)), ImGuiChildFlags_AutoResizeX)) {
            for (auto [id, partial] : m_ui.preset_add.partials | std::views::enumerate) {
                ImGui::PushID(int(id));

                if (ImGui::Button("X")) {
                    m_task_manager.add_immediate_task([this, index = id] {
                        m_ui.preset_add.partials.erase(std::next(m_ui.preset_add.partials.begin(), index));
                    });
                }

                ImGui::SameLine();

                ImGui::Text("%ld.", id + 1);

                ImGui::SameLine();

                ImGui::PushItemWidth(ui::rem(7.0f));

                constexpr const char* OSCILLATOR_TYPE[] { "Sine", "Square", "Triangle", "Sawtooth", "Noise" };

                if (ImGui::BeginCombo("##Oscillator", OSCILLATOR_TYPE[partial.oscillator_type])) {
                    for (std::size_t i {}; i < std::size(OSCILLATOR_TYPE); i++) {
                        if (ImGui::Selectable(OSCILLATOR_TYPE[i], partial.oscillator_type == int(i))) {
                            partial.oscillator_type = ui::preset::OscillatorType(i);
                        }
                    }

                    ImGui::EndCombo();
                }

                ImGui::SetItemTooltip("Oscillator");

                ImGui::PopItemWidth();

                ImGui::SameLine();

                ImGui::PushItemWidth(ui::rem(5.0f));

                const bool noise_type = partial.oscillator_type == ui::preset::OscillatorTypeNoise;

                if (!noise_type) {
                    ImGui::DragScalar("##Frequency Multiplier", ImGuiDataType_Double, &partial.frequency_multiplier, DRAG_SPEED_FAST, &POINT_ONE, &FIFTEEN, "%.3f", ImGuiSliderFlags_ClampOnInput);

                    ImGui::SetItemTooltip("Frequency Multiplier");

                    ImGui::SameLine();
                }

                ImGui::DragScalar("##Amplitude Divisor", ImGuiDataType_Double, &partial.amplitude_divisor, DRAG_SPEED_FAST, &ONE, &ONE_HUNDRED, "%.3f", ImGuiSliderFlags_ClampOnInput);

                ImGui::SetItemTooltip("Amplitude Divisor");

                if (!noise_type) {
                    ImGui::SameLine();

                    ImGui::DragScalar("##Phase", ImGuiDataType_Double, &partial.phase, DRAG_SPEED_SLOW, &ZERO, &math::TWO_PI, "%.3f", ImGuiSliderFlags_ClampOnInput);

                    ImGui::SetItemTooltip("Phase");

                    ImGui::SameLine();

                    ImGui::Checkbox("##LFO", &partial.lfo.enabled);

                    ImGui::SetItemTooltip("LFO");

                    if (partial.lfo.enabled) {
                        ImGui::SameLine();

                        ImGui::DragScalar("##LFO Frequency", ImGuiDataType_Double, &partial.lfo.frequency, DRAG_SPEED_FAST, &ONE, &TWENTY, "%.3f", ImGuiSliderFlags_ClampOnInput);

                        ImGui::SetItemTooltip("LFO Frequency");

                        ImGui::SameLine();

                        ImGui::DragScalar("##LFO Deviation", ImGuiDataType_Double, &partial.lfo.deviation, DRAG_SPEED_SLOW, &ZERO, &ONE, "%.3f", ImGuiSliderFlags_ClampOnInput);

                        ImGui::SetItemTooltip("LFO Deviation");
                    }
                }

                ImGui::PopItemWidth();

                ImGui::Indent();

                if (ImGui::TreeNode("Envelope")) {
                    create_instrument_envelope(partial.envelope);
                    ImGui::TreePop();
                }

                ImGui::Unindent();

                ImGui::PopID();
            }
        }

        ImGui::EndChild();
    }

    void Application::create_instrument_pad() {
        ImGui::Dummy(ui::rem(ImVec2(0.0f, 0.5f)));

        constexpr const char* PROFILE[] { "Gaussian", "Even", "Single Sine", "Double Sine", "Arc" };

        if (ImGui::BeginCombo("Profile", PROFILE[m_ui.preset_pad.profile])) {
            for (std::size_t i {}; i < std::size(PROFILE); i++) {
                if (ImGui::Selectable(PROFILE[i], m_ui.preset_pad.profile == int(i))) {
                    m_ui.preset_pad.profile = ui::preset::Profile(i);
                }
            }

            ImGui::EndCombo();
        }

        ImGui::DragScalar("Frequency", ImGuiDataType_Double, &m_ui.preset_pad.frequency, DRAG_SPEED_FAST, &syn::FREQUENCY_MIN, &syn::FREQUENCY_MAX, "%.3f", ImGuiSliderFlags_ClampOnInput);

        ImGui::DragScalar("Bandwidth", ImGuiDataType_Double, &m_ui.preset_pad.bandwidth, DRAG_SPEED_FAST, &POINT_ONE, &ONE_HUNDRED, "%.3f", ImGuiSliderFlags_ClampOnInput);  // TODO which values?

        ImGui::Dummy(ui::rem(ImVec2(0.0f, 0.5f)));

        if (ImGui::Button("Add Amplitude")) {
            m_ui.preset_pad.amplitude_harmonics.push_back(ui::preset::PresetPad::DEFAULT_AMPLITUDE);
        }

        if (ImGui::BeginChild("Amplitude Harmonics", ImVec2(0.0f, ui::rem(18.0f)), ImGuiChildFlags_AutoResizeX)) {
            for (auto [id, amplitude_harmonic] : m_ui.preset_pad.amplitude_harmonics | std::views::enumerate) {
                ImGui::PushID(int(id));

                if (ImGui::Button("X")) {
                    m_task_manager.add_immediate_task([this, index = id] {
                        m_ui.preset_pad.amplitude_harmonics.erase(std::next(m_ui.preset_pad.amplitude_harmonics.begin(), index));
                    });
                }

                ImGui::SameLine();

                ImGui::Text("%ld.", id + 1);

                ImGui::SameLine();

                ImGui::DragScalar("##Amplitude", ImGuiDataType_Double, &amplitude_harmonic, DRAG_SPEED_SLOW, &ZERO, &ONE, "%.3f", ImGuiSliderFlags_ClampOnInput);

                ImGui::PopID();
            }
        }

        ImGui::EndChild();
    }

    void Application::create_instrument_buttons_add() {
        ImGui::Dummy(ui::rem(ImVec2(0.0f, 0.5f)));

        ImGui::BeginDisabled(*m_ui.preset_add.name == 0);

        if (ImGui::Button("Store into Synthesizer")) {
            const bool inserted = m_synthesizer.store_instrument(std::make_unique<preset::add::RuntimeInstrument>(translate_preset(m_ui.preset_add)));
            set_composition_instrument_colors();

            if (inserted) {
                LOG_DEBUG("Created a new runtime instrument");
                notify_message("Created a new runtime instrument");
            } else {
                LOG_DEBUG("Updated the runtime instrument");
                notify_message("Updated the runtime instrument");
            }
        }

        ImGui::SameLine();

        if (ImGui::Button("Save to File")) {
            preset_file_save_add();
        }

        ImGui::EndDisabled();
    }

    void Application::create_instrument_buttons_pad() {
        ImGui::Dummy(ui::rem(ImVec2(0.0f, 0.5f)));

        ImGui::BeginDisabled(*m_ui.preset_pad.name == 0);

        if (ImGui::Button("Store into Synthesizer")) {
            const bool inserted = m_synthesizer.store_instrument(std::make_unique<preset::pad::RuntimeInstrument>(translate_preset(m_ui.preset_pad)));
            set_composition_instrument_colors();

            if (inserted) {
                LOG_DEBUG("Created a new runtime instrument");
                notify_message("Created a new runtime instrument");
            } else {
                LOG_DEBUG("Updated the runtime instrument");
                notify_message("Updated the runtime instrument");
            }
        }

        ImGui::SameLine();

        if (ImGui::Button("Save to File")) {
            preset_file_save_pad();
        }

        ImGui::EndDisabled();
    }

    void Application::render_composition() {
        window_menu("Render Composition", m_render_composition_menu, [this] {
            ImGui::Text("Render and export to a raw WAV file:");

            ImGui::Dummy(ui::rem(ImVec2(0.0f, 0.5f)));

            ImGui::SetNextItemWidth(ui::rem(32.0f));

            ImGui::InputText("File Path", m_ui.render_file_path, sizeof(m_ui.render_file_path), ImGuiInputTextFlags_ElideLeft);

            ImGui::Dummy(ui::rem(ImVec2(0.0f, 0.5f)));

            ImGui::ProgressBar(m_ui.render_progress, ImVec2(0.0f, 0.0f));

            ImGui::SameLine();

            ImGui::BeginDisabled(m_render_in_progress || *m_ui.render_file_path == 0);

            ImGui::Checkbox("Normalize", &m_ui.render_normalize);

            ImGui::SameLine();

            if (ImGui::Button("Render")) {
                start_render_composition();
            }

            ImGui::EndDisabled();
        });
    }

    void Application::messages() {
        for (const auto& [i, message] : m_messages.messages | std::views::reverse | std::views::enumerate) {
            static constexpr auto flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
            const auto size = ui::rem(ImVec2(18.0f, 5.0f));
            const float offset_y = float(i) * (size.y + ui::rem(0.5f));

            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->Size - size - ui::rem(ImVec2(0.5f, 0.5f)) - ImVec2(0.0f, offset_y), ImGuiCond_Always);
            ImGui::SetNextWindowSize(size);

            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, message.opacity);

            if (ImGui::Begin(("Message"s + std::to_string(message.sequence)).c_str(), nullptr, flags)) {
                ImGui::TextWrapped("%s", message.message.c_str());

                if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    erase_message(message);
                }
            }

            ImGui::End();

            ImGui::PopStyleVar();
            ImGui::PopStyleVar();
        }
    }

    void Application::debug() const {
#ifndef ALFRED_DISTRIBUTION
        if (ImGui::Begin("Debug", nullptr, ImGuiWindowFlags_NoDocking)) {
            ImGui::Text("frame_time() %f", frame_time());
            ImGui::Text("m_synthesizer.current_voices() %zu", m_synthesizer.current_voices());
            ImGui::Text("m_synthesizer.time() %.03f", m_synthesizer.time());
            ImGui::Text("m_player.get_position() %u", m_player.position());
            ImGui::Text("m_composition_not_compiled %d", m_composition_not_compiled);
            ImGui::Text("m_composition_not_saved %d", m_composition_not_saved);
            ImGui::Text("m_composition_history.undo.size() %zu", m_composition_history.undo.size());
            ImGui::Text("m_composition_history.redo.size() %zu", m_composition_history.redo.size());
        }

        ImGui::End();
#endif
    }

    void Application::window_menu(const char* name, bool& open, const std::function<void()>& window) {
        if (!open) {
            return;
        }

        ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->Size.x / 2.0f, ImGui::GetMainViewport()->Size.y / 2.0f), ImGuiCond_Once, ImVec2(0.5f, 0.5f));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);

        static constexpr auto flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking;

        if (ImGui::Begin(name, &open, flags)) {
            window();
        }

        ImGui::End();

        ImGui::PopStyleVar();
    }

    void Application::keyboard_input(unsigned int key, bool down) {
        auto update = [this, down](syn::NoteId id) {
            if (down) {
                m_synthesizer.note_on(id + m_octave * 12, m_instrument, seq::amplitude(seq::Loudness::Fortississimo));
            } else {
                m_synthesizer.note_off(id + m_octave * 12, m_instrument);
            }
        };

        switch (key) {
            case SDLK_Z: update(0); break;
            case SDLK_S: update(1); break;
            case SDLK_X: update(2); break;
            case SDLK_C: update(3); break;
            case SDLK_F: update(4); break;
            case SDLK_V: update(5); break;
            case SDLK_G: update(6); break;
            case SDLK_B: update(7); break;
            case SDLK_N: update(8); break;
            case SDLK_J: update(9); break;
            case SDLK_M: update(10); break;
            case SDLK_K: update(11); break;
            case SDLK_COMMA: update(12); break;
            case SDLK_L: update(13); break;
            case SDLK_PERIOD: update(14); break;
            case SDLK_SLASH: update(15); break;
        }
    }

    void Application::composition_mouse_pressed(ImVec2 origin) {
        if (ImGui::IsKeyDown(ImGuiMod_Alt)) {
            if (const auto position = hover_position(composition_mouse_position(origin)); position) {
                seek_player(*position);
            }
            return;
        }

        switch (m_ui.tool) {
            case ui::ToolMeasure: {
                if (const auto hovered_measure = hover_measure(composition_mouse_position(origin)); hovered_measure) {
                    toggle_select_measure(*hovered_measure);
                }
                break;
            }
            case ui::ToolNote: {
                if (const auto hovered_note = hover_note(composition_mouse_position(origin)); hovered_note) {
                    do_with_note(*hovered_note);
                }
                break;
            }
        }
    }

    void Application::composition_camera(bool item_active, bool item_hovered, ImVec2 space) {
        const auto& io = ImGui::GetIO();

        if (item_active && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            m_composition_camera -= io.MouseDelta;
        }

        if (item_hovered) {
            if (ImGui::IsKeyDown(ImGuiMod_Shift)) {
                m_composition_camera -= ImVec2(
                    ui::rem(COMPOSITION_SCROLL_SPEED * 2.0f) * io.MouseWheel,
                    -ui::rem(COMPOSITION_SCROLL_SPEED) * io.MouseWheelH
                );
            } else {
                m_composition_camera -= ImVec2(
                    -ui::rem(COMPOSITION_SCROLL_SPEED * 2.0f) * io.MouseWheelH,
                    ui::rem(COMPOSITION_SCROLL_SPEED) * io.MouseWheel
                );
            }
        }

        const float cursor_position = float(m_player.position()) * ui::rem(STEP_SIZE.x) - m_composition_camera.x;

        if (m_player.playing() && cursor_position > space.x / 2.0f) {
            m_composition_camera.x += std::floor(cursor_position - space.x / 2.0f);
        }

        m_composition_camera.x = std::max(m_composition_camera.x, 0.0f);
        m_composition_camera.x = std::min(m_composition_camera.x, composition_width());
        m_composition_camera.y = std::max(m_composition_camera.y, 0.0f);
        m_composition_camera.y = std::min(m_composition_camera.y, std::max(ui::rem(COMPOSITION_HEIGHT + STEP_SIZE.y) - space.y, 0.0f));
    }

    void Application::toggle_select_measure(MeasureIter measure) {
        if (m_composition_selected_measure == measure) {
            m_composition_selected_measure = m_composition.measures.end();
        } else {
            m_composition_selected_measure = measure;
            select_measure(measure);
        }
    }

    void Application::select_measure(MeasureIter measure) {
        set_dynamics(m_ui.dynamics, *measure);
        set_agogic(m_ui.agogic, *measure);
        set_time_signature(m_ui.time_signature, *measure);
    }

    void Application::append_measures() {
        assert(!m_player.playing());

        remember_composition();

        seq::TimeSignature time_signature;
        seq::Dynamics dynamics;
        seq::Agogic agogic;

        measure_properties(
            !m_composition.measures.empty() ? std::prev(m_composition.measures.end()) : m_composition.measures.end(),
            m_composition.measures,
            time_signature,
            dynamics,
            agogic
        );

        for (int i {}; i < ADD_MEASURES; i++) {
            m_composition.measures.emplace_back(time_signature, dynamics, agogic);
        }

        m_composition_selected_measure = m_composition.measures.end();

        modify_composition();
    }

    void Application::insert_measure() {
        assert(!m_player.playing());
        assert(m_composition_selected_measure != m_composition.measures.end());

        remember_composition();

        seq::TimeSignature time_signature;
        seq::Dynamics dynamics;
        seq::Agogic agogic;

        measure_properties(
            m_composition_selected_measure,
            m_composition.measures,
            time_signature,
            dynamics,
            agogic
        );

        reset_note_legato_previous_measure(m_composition_selected_measure);

        m_composition_selected_measure = m_composition.measures.emplace(std::next(m_composition_selected_measure), time_signature, dynamics, agogic);

        modify_composition();

        // The selected measure has changed
        select_measure(m_composition_selected_measure);
    }

    void Application::clear_measure() {
        assert(!m_player.playing());
        assert(m_composition_selected_measure != m_composition.measures.end());

        remember_composition();

        reset_note_legato_previous_measure(m_composition_selected_measure);

        m_composition_selected_measure->instruments.clear();

        modify_composition();
    }

    void Application::delete_measure() {
        assert(!m_player.playing());
        assert(m_composition_selected_measure != m_composition.measures.end());

        remember_composition();

        reset_note_legato_previous_measure(m_composition_selected_measure);

        m_composition_selected_measure = m_composition.measures.erase(m_composition_selected_measure);

        modify_composition();

        // The selected measure has changed
        if (m_composition_selected_measure != m_composition.measures.end()) {
            select_measure(m_composition_selected_measure);
        }

        keep_player_cursor_valid();
    }

    void Application::set_measure_dynamics() {
        assert(!m_player.playing());
        assert(m_composition_selected_measure != m_composition.measures.end());

        remember_composition();

        set_dynamics(*m_composition_selected_measure, m_ui.dynamics);

        modify_composition();
    }

    void Application::set_measure_agogic() {
        assert(!m_player.playing());
        assert(m_composition_selected_measure != m_composition.measures.end());

        remember_composition();

        set_agogic(*m_composition_selected_measure, m_ui.agogic);

        modify_composition();
    }

    void Application::set_measure_time_signature() {
        assert(!m_player.playing());
        assert(m_composition_selected_measure != m_composition.measures.end());

        if (measure_empty(*m_composition_selected_measure)) {
            remember_composition();

            set_time_signature(*m_composition_selected_measure, m_ui.time_signature);

            modify_composition();
        } else {
            // Reset back
            set_time_signature(m_ui.time_signature, *m_composition_selected_measure);
            LOG_DEBUG("Cannot change time signature in this state");
            notify_message("Cannot change time signature in this state");
        }
    }

    std::optional<MeasureIter> Application::hover_measure(ImVec2 position) {
        float position_x {};

        for (auto measure = m_composition.measures.begin(); measure != m_composition.measures.end(); measure++) {
            const float right = float(measure->time_signature.measure_steps()) * ui::rem(STEP_SIZE.x);

            if (position.x >= position_x && position.x < position_x + right) {
                return std::make_optional(measure);
            }

            position_x += right;
        }

        return std::nullopt;
    }

    std::optional<HoveredNote> Application::hover_note(ImVec2 position) {
        syn::NoteId result_id {};

        {
            const int index = int(position.y / ui::rem(STEP_SIZE.y));
            const int id = syn::keyboard::NOTES - 1 - index;

            if (id < 0) {
                return std::nullopt;  // This can happen now when hovering the mouse below the lowest note
            }

            result_id = syn::NoteId(id);
        }

        float position_x {};
        std::uint32_t global_position {};

        for (auto measure = m_composition.measures.begin(); measure != m_composition.measures.end(); measure++) {
            const float right = float(measure->time_signature.measure_steps()) * ui::rem(STEP_SIZE.x);

            if (position.x >= position_x && position.x < position_x + right) {
                const float offset = position.x - position_x;
                const std::uint32_t result_position = std::uint32_t(offset / ui::rem(STEP_SIZE.x));

                return std::make_optional<HoveredNote>(
                    result_id,
                    measure,
                    result_position,
                    global_position + result_position
                );
            }

            position_x += right;
            global_position += measure->time_signature.measure_steps();
        }

        return std::nullopt;
    }

    std::optional<NoteIter> Application::select_note(const HoveredNote& hovered_note) const {
        const auto instrument = hovered_note.measure()->instruments.find(m_instrument);

        if (instrument == hovered_note.measure()->instruments.end()) {
            return std::nullopt;
        }

        for (auto n = instrument->second.begin(); n != instrument->second.end(); n++) {
            if (hovered_note.id() == n->id) {
                if (hovered_note.position() >= n->position && hovered_note.position() < n->position + seq::steps(n->value, n->tuplet)) {
                    return std::make_optional(n);
                }
            }
        }

        return std::nullopt;
    }

    std::optional<std::uint32_t> Application::hover_position(ImVec2 position) const {
        std::uint32_t result {};

        for (float position_x {}; const seq::Measure& measure : m_composition.measures) {
            const float right = float(measure.time_signature.measure_steps()) * ui::rem(STEP_SIZE.x);

            if (position.x >= position_x && position.x < position_x + right) {
                const float offset = position.x - position_x;

                result += std::uint32_t(offset / ui::rem(STEP_SIZE.x));

                return std::make_optional(result);
            }

            position_x += right;
            result += measure.time_signature.measure_steps();
        }

        return std::nullopt;
    }


    void Application::do_with_note(const HoveredNote& hovered_note) {
        if (auto note = select_note(hovered_note); note) {
            const auto selected_note {
                std::ranges::find_if(m_composition_selected_notes, [this, &hovered_note, note](const auto& n) {
                    return n.measure() == hovered_note.measure() && n.note() == *note && n.instrument() == m_instrument;
                })
            };

            if (ImGui::IsKeyDown(ImGuiMod_Ctrl)) {
                if (selected_note != m_composition_selected_notes.end()) {
                    m_composition_selected_notes.erase(selected_note);
                } else {
                    m_composition_selected_notes.emplace_back(hovered_note.measure(), *note, m_instrument);
                }
            } else {
                const bool exists = selected_note != m_composition_selected_notes.end();

                m_composition_selected_notes.clear();

                if (!exists) {
                    m_composition_selected_notes.emplace_back(hovered_note.measure(), *note, m_instrument);
                }
            }

            return;
        }

        if (!m_composition_selected_notes.empty()) {
            m_composition_selected_notes.clear();
            return;
        }

        if (m_ui.tuplet != ui::TupletNone && m_ui.value == ui::ValueSixteenth) {
            LOG_DEBUG("Cannot create a tuplet out of a sixteenth note");
            notify_message("Cannot create a tuplet out of a sixteenth note");
            return;
        }

        const auto new_note_value = translate_value(ui::Value(m_ui.value));

        const seq::Note new_note {
            hovered_note.id(),
            new_note_value,
            hovered_note.position() / seq::DIVISION * seq::DIVISION,  // Always place on groups of steps
            seq::Tuplet::None
        };

        // Test this particular new note, even though tuplets are inserted as something different
        if (
            ![this, &hovered_note, &new_note] {
                if (new_note.position + seq::steps(new_note.value) > hovered_note.measure()->time_signature.measure_steps()) {
                    return false;
                }

                for (const seq::Note& note : hovered_note.measure()->instruments[m_instrument]) {
                    if (notes_overlapping(note, new_note)) {
                        return false;
                    }
                }

                return true;
            }()
        ) {
            LOG_DEBUG("Cannot place note here");
            notify_message("Cannot place note here");
            return;
        }

        remember_composition();

        switch (m_ui.tuplet) {
            case ui::TupletNone:
                hovered_note.measure()->instruments[m_instrument].insert(new_note);
                break;
            case ui::TupletTriplet:
                hovered_note.measure()->instruments[m_instrument].emplace(
                    hovered_note.id(),
                    new_note_value,
                    hovered_note.position() / seq::DIVISION * seq::DIVISION,
                    seq::Tuplet::Triplet
                );
                hovered_note.measure()->instruments[m_instrument].emplace(
                    hovered_note.id(),
                    new_note_value,
                    hovered_note.position() / seq::DIVISION * seq::DIVISION + seq::steps(new_note_value, seq::Tuplet::Triplet),
                    seq::Tuplet::Triplet
                );
                hovered_note.measure()->instruments[m_instrument].emplace(
                    hovered_note.id(),
                    new_note_value,
                    hovered_note.position() / seq::DIVISION * seq::DIVISION + 2 * seq::steps(new_note_value, seq::Tuplet::Triplet),
                    seq::Tuplet::Triplet
                );
                break;
        }

        play_note(m_instrument, new_note);

        modify_composition();
    }

    void Application::delete_notes() {
        assert(!m_player.playing());
        assert(!m_composition_selected_notes.empty());

        remember_composition();

        for (const ProvenanceNote& selected_note : m_composition_selected_notes) {
            if (auto previous_note = check_note_has_previous(selected_note); previous_note) {
                reset_note_legato(*previous_note);
            }

            selected_note.measure()->instruments.at(m_instrument).erase(selected_note.note());
        }

        m_composition_selected_notes.clear();

        modify_composition();
    }

    void Application::legato_notes() {
        assert(!m_player.playing());
        assert(!m_composition_selected_notes.empty());

        remember_composition();

        for (ProvenanceNote& selected_note : m_composition_selected_notes) {
            seq::Note note = selected_note.copy();
            note.legato = !note.legato;

            if (note.legato) {
                if (const auto next_note = check_note_has_next(selected_note); next_note) {
                    if (next_note->note()->delay > 0) {
                        LOG_DEBUG("Note cannot have legato");
                        notify_message("Note cannot have legato");
                        continue;
                    }
                } else {
                    LOG_DEBUG("Note cannot have legato");
                    notify_message("Note cannot have legato");
                    continue;
                }
            }

            readd_note(selected_note, note);
        }

        modify_composition();
    }

    void Application::shift_notes_up() {  // FIXME don't reset legato if the tied notes are both shifted
        assert(!m_player.playing());
        assert(!m_composition_selected_notes.empty());

        if (
            ![this] {
                for (const ProvenanceNote& selected_note : m_composition_selected_notes) {
                    if (!check_note_up_limit(*selected_note.note())) {
                        return false;
                    }

                    const auto& notes = selected_note.measure()->instruments.at(m_instrument);

                    for (auto note = notes.begin(); note != notes.end(); note++) {
                        if (note_in_selection(note, selected_note.measure(), m_composition_selected_notes)) {
                            continue;
                        }

                        seq::Note shifted_note = selected_note.copy();
                        shifted_note.id++;

                        if (notes_overlapping(*note, shifted_note)) {
                            return false;
                        }
                    }
                }

                return true;
            }()
        ) {
            LOG_DEBUG("Cannot shift notes here");
            notify_message("Cannot shift notes here");
            return;
        }

        remember_composition();

        for (ProvenanceNote& selected_note : m_composition_selected_notes) {
            seq::Note note = selected_note.copy();
            note.id++;
            note.legato = false;

            if (const auto previous_note = check_note_has_previous(selected_note); previous_note) {
                reset_note_legato(*previous_note);
            }

            readd_note(selected_note, note);
            play_note(selected_note.instrument(), note);
        }

        modify_composition();
    }

    void Application::shift_notes_down() {  // FIXME don't reset legato if the tied notes are both shifted
        assert(!m_player.playing());
        assert(!m_composition_selected_notes.empty());

        if (
            ![this] {
                for (const ProvenanceNote& selected_note : m_composition_selected_notes) {
                    if (!check_note_down_limit(*selected_note.note())) {
                        return false;
                    }

                    const auto& notes = selected_note.measure()->instruments.at(m_instrument);

                    for (auto note = notes.begin(); note != notes.end(); note++) {
                        if (note_in_selection(note, selected_note.measure(), m_composition_selected_notes)) {
                            continue;
                        }

                        seq::Note shifted_note = selected_note.copy();
                        shifted_note.id--;

                        if (notes_overlapping(*note, shifted_note)) {
                            return false;
                        }
                    }
                }

                return true;
            }()
        ) {
            LOG_DEBUG("Cannot shift notes here");
            notify_message("Cannot shift notes here");
            return;
        }

        remember_composition();

        for (ProvenanceNote& selected_note : m_composition_selected_notes) {
            seq::Note note = selected_note.copy();
            note.id--;
            note.legato = false;

            if (const auto previous_note = check_note_has_previous(selected_note); previous_note) {
                reset_note_legato(*previous_note);
            }

            readd_note(selected_note, note);
            play_note(selected_note.instrument(), note);
        }

        modify_composition();
    }

    void Application::shift_notes_left() {
        assert(!m_player.playing());
        assert(!m_composition_selected_notes.empty());

        if (
            ![this] {
                for (const ProvenanceNote& selected_note : m_composition_selected_notes) {
                    if (selected_note.note()->tuplet != seq::Tuplet::None) {
                        return false;
                    }

                    if (!check_note_left_limit(*selected_note.note())) {
                        return false;
                    }

                    const auto& notes = selected_note.measure()->instruments.at(m_instrument);

                    for (auto note = notes.begin(); note != notes.end(); note++) {
                        if (note_in_selection(note, selected_note.measure(), m_composition_selected_notes)) {
                            continue;
                        }

                        seq::Note shifted_note = selected_note.copy();
                        shifted_note.position--;

                        if (notes_overlapping(*note, shifted_note)) {
                            return false;
                        }
                    }
                }

                return true;
            }()
        ) {
            LOG_DEBUG("Cannot shift notes here");
            notify_message("Cannot shift notes here");
            return;
        }

        remember_composition();

        for (ProvenanceNote& selected_note : m_composition_selected_notes) {
            seq::Note note = selected_note.copy();
            note.position -= seq::DIVISION;
            note.legato = false;  // No need to check previous

            readd_note(selected_note, note);
        }

        modify_composition();
    }

    void Application::shift_notes_right() {
        assert(!m_player.playing());
        assert(!m_composition_selected_notes.empty());

        if (
            ![this] {
                for (const ProvenanceNote& selected_note : m_composition_selected_notes) {
                    if (selected_note.note()->tuplet != seq::Tuplet::None) {
                        return false;
                    }

                    if (!check_note_right_limit(*selected_note.note(), *selected_note.measure())) {
                        return false;
                    }

                    const auto& notes = selected_note.measure()->instruments.at(m_instrument);

                    for (auto note = notes.begin(); note != notes.end(); note++) {
                        if (note_in_selection(note, selected_note.measure(), m_composition_selected_notes)) {
                            continue;
                        }

                        seq::Note shifted_note = selected_note.copy();
                        shifted_note.position++;

                        if (notes_overlapping(*note, shifted_note)) {
                            return false;
                        }
                    }
                }

                return true;
            }()
        ) {
            LOG_DEBUG("Cannot shift notes here");
            notify_message("Cannot shift notes here");
            return;
        }

        remember_composition();

        for (ProvenanceNote& selected_note : m_composition_selected_notes) {
            seq::Note note = selected_note.copy();
            note.position += seq::DIVISION;
            note.legato = false;

            if (const auto previous_note = check_note_has_previous(selected_note); previous_note) {
                reset_note_legato(*previous_note);
            }

            readd_note(selected_note, note);
        }

        modify_composition();
    }

    void Application::add_delay_notes() {
        assert(!m_player.playing());
        assert(!m_composition_selected_notes.empty());

        remember_composition();

        for (ProvenanceNote& selected_note : m_composition_selected_notes) {
            if (
                selected_note.note()->delay == seq::MAX_DELAY ||
                seq::steps(selected_note.note()->value, selected_note.note()->tuplet) - selected_note.note()->delay < seq::DIVISION
            ) {
                LOG_DEBUG("Cannot add any more delay to note");
                notify_message("Cannot add any more delay to note");
                continue;
            }

            if (const auto previous_note = check_note_has_previous(selected_note); previous_note) {
                if (previous_note->note()->legato) {
                    LOG_DEBUG("Cannot add delay to note");
                    notify_message("Cannot add delay to note");
                    continue;
                }
            }

            seq::Note note = selected_note.copy();
            note.delay += seq::DELAY_INCREMENT;

            readd_note(selected_note, note);
        }

        modify_composition();
    }

    void Application::remove_delay_notes() {
        assert(!m_player.playing());
        assert(!m_composition_selected_notes.empty());

        remember_composition();

        for (ProvenanceNote& selected_note : m_composition_selected_notes) {
            if (selected_note.note()->delay == 0) {
                LOG_DEBUG("Cannot remove any more delay from note");
                notify_message("Cannot remove any more delay from note");
                continue;
            }

            seq::Note note = selected_note.copy();
            note.delay -= seq::DELAY_INCREMENT;

            readd_note(selected_note, note);
        }

        modify_composition();
    }

    void Application::start_player() {
        if (m_composition_not_compiled) {
            LOG_DEBUG("Compiling composition");
            m_player.prepare();
            m_composition_not_compiled = false;
        }

        reset_composition_selection();

        m_synthesizer.polyphony(optimal_composition_voices(m_composition));
        LOG_DEBUG("Set polyphony to {}", m_synthesizer.polyphony());

        set_synthesizer_instrument_volumes(m_synthesizer);
        desired_frame_time(FRAME_TIME_PLAYBACK);
        m_player.start();
    }

    void Application::stop_player() {  // FIXME stopping the player doesn't instantly stop the synthesizer voices, but we instantly change volume and polyphony
        m_player.stop();
        desired_frame_time(FRAME_TIME_DEFAULT);
        reset_synthesizer_instrument_volumes(m_synthesizer);

        m_synthesizer.polyphony(std::size_t(m_ui.polyphony));
        LOG_DEBUG("Reset polyphony to {}", m_synthesizer.polyphony());
    }

    void Application::seek_player(std::uint32_t position) {
        if (m_player.playing()) {
            stop_player();
        }

        m_player.seek(position);
    }

    void Application::modify_composition() {
        if (!m_composition_not_saved) {
            set_title_composition_not_saved();
        }

        m_composition_not_compiled = true;
        m_composition_not_saved = true;
    }

    void Application::modify_composition_metadata() {
        if (!m_composition_not_saved) {
            set_title_composition_not_saved();
        }

        m_composition_not_saved = true;
    }

    void Application::invalidate_composition() {
        m_composition_not_compiled = true;
    }

    void Application::reset_composition_flags() {
        m_composition_not_compiled = false;
        m_composition_not_saved = false;
    }

    void Application::reset_composition_selection() {
        m_composition_selected_measure = m_composition.measures.end();
        m_composition_selected_notes.clear();
    }

    void Application::reset_everything() {
        seek_player(0);
        reset_composition_selection();
        m_composition_camera = {};
        m_composition_history = {};
    }

    void Application::set_title_composition_not_saved() const {
        if (m_composition_path.empty()) {
            title("Alfred | Unsaved Composition");
        } else {
            title(std::format("Alfred | {}*", m_composition_path.filename().string()));
        }

        LOG_DEBUG("Changed title");
    }

    void Application::set_title_composition_saved() const {
        if (m_composition_path.empty()) {
            title("Alfred");
        } else {
            title(std::format("Alfred | {}", m_composition_path.filename().string()));
        }

        LOG_DEBUG("Changed title");
    }

    void Application::set_color_scheme(ui::ColorScheme color_scheme) {
        ui::set_color_scheme(color_scheme);

        LOG_DEBUG("Changed color scheme");
    }

    void Application::set_scale(ui::Scale scale) {
        ui::set_scale(ui::scale(scale));
        m_invalidate_ui_dock_builder = true;

        LOG_DEBUG("Changed scale");
    }

    float Application::composition_width() const {
        return float(m_composition.size()) * ui::rem(STEP_SIZE.x);
    }

    ImVec2 Application::composition_space(ImVec2 space) const {
        const float max_width = composition_width() - m_composition_camera.x;
        const float max_height = ui::rem(COMPOSITION_HEIGHT) - m_composition_camera.y;

        return { std::min(space.x, max_width), std::min(space.y, max_height) };
    }

    ImVec2 Application::composition_mouse_position(ImVec2 origin) const {
        const ImVec2 mouse_position = ImGui::GetIO().MousePos;

        if (!ImGui::IsMousePosValid(&mouse_position)) {
            return ImVec2(0.0f, 0.0f);
        }

        return mouse_position - origin - ImVec2(ui::rem(COMPOSITION_LEFT), 0.0f) + m_composition_camera;
    }

    std_flat_set<syn::InstrumentId> Application::active_instruments() const {
        std_flat_set<syn::InstrumentId> instruments;

        for (const seq::Measure& measure : m_composition.measures) {
            for (const auto& instrument : measure.instruments) {
                if (!instrument.second.empty()) {
                    instruments.insert(instrument.first);
                }
            }
        }

        return instruments;
    }

    bool Application::point_x_in_camera_view(float point_x, float space_x) const {
        return point_x >= m_composition_camera.x && point_x < m_composition_camera.x + space_x;
    }

    bool Application::point_y_in_camera_view(float point_y, float space_y) const {
        return point_y >= m_composition_camera.y && point_y < m_composition_camera.y + space_y;
    }

    void Application::readd_note(ProvenanceNote& provenance_note, const seq::Note& note) {
        provenance_note.measure()->instruments.at(provenance_note.instrument()).erase(provenance_note.note());
        const auto [iter, inserted] {provenance_note.measure()->instruments.at(provenance_note.instrument()).insert(note)};

        assert(inserted);

        provenance_note.note(iter);
    }

    void Application::readd_note(syn::InstrumentId instrument, NoteIter note_iter, MeasureIter measure, const seq::Note& note) {
        measure->instruments.at(instrument).erase(note_iter);
        measure->instruments.at(instrument).insert(note);
    }

    void Application::reset_note_legato(const ProvenanceNote& provenance_note) {
        seq::Note note = provenance_note.copy();
        note.legato = false;

        readd_note(provenance_note.instrument(), provenance_note.note(), provenance_note.measure(), note);
    }

    void Application::reset_note_legato_previous_measure(MeasureIter measure) const {
        for (const auto& [instrument, notes] : measure->instruments) {
            for (auto note = notes.begin(); note != notes.end(); note++) {
                if (seq::Composition::note_first_in_measure(*measure, *note)) {
                    if (auto previous_note = m_composition.check_note_has_previous(measure, note, instrument); previous_note) {
                        reset_note_legato(*previous_note);
                    }
                }
            }
        }
    }

    void Application::play_note(syn::InstrumentId instrument, const seq::Note& note) {
        m_synthesizer.note_on(note.id, instrument, seq::amplitude(seq::Loudness::MezzoForte));

        m_task_manager.add_delayed_task([this, instrument = instrument, id = note.id] {
            m_synthesizer.note_off(id, instrument);
        }, math::seconds_to_milliseconds(m_synthesizer.get_instrument(instrument).attack_duration()) + 100);
    }

    void Application::note_to_string(syn::NoteId note, char* buffer) {
        constexpr const char* NOTES[] { "A \0", "A #", "B \0", "C \0", "C #", "D \0", "D #", "E \0", "F \0", "F #", "G \0", "G #" };

        const auto [name, octave] {syn::note(note)};

        std::strncpy(buffer, NOTES[name], 3);
        buffer[1] = char(octave + 48);
    }

    bool Application::keyboard_active() {
        return !imgui::want_capture_keyboard() && !ImGui::IsKeyDown(ImGuiMod_Ctrl) && !ImGui::IsKeyDown(ImGuiMod_Alt) && !ImGui::IsKeyDown(ImGuiMod_Shift);
    }

    float Application::note_height(const seq::Note& note) {
        return ui::rem(COMPOSITION_HEIGHT) - ui::rem(STEP_SIZE.y) - float(note.id) * ui::rem(STEP_SIZE.y);
    }

    ImVec4 Application::note_rectangle(const seq::Note& note) {
        const float x = float(note.position + note.delay) * ui::rem(STEP_SIZE.x);
        const float y = note_height(note);
        const float width = float(seq::steps(note.value, note.tuplet) - note.delay) * ui::rem(STEP_SIZE.x);
        const float height = ui::rem(STEP_SIZE.y);

        return ImVec4(x + 1.0f, y + 2.0f, width - 2.0f, height - 4.0f);
    }

    const char* Application::measure_label(char* buffer, long number) {
        const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), number);

        if (result.ec != std::errc()) {
            std::strcpy(buffer, "?");
        } else {
            *result.ptr = '\0';
        }

        return buffer;
    }

    void Application::measure_properties(MeasureIter measure, const std::vector<seq::Measure>& measures, seq::TimeSignature& time_signature, seq::Dynamics& dynamics, seq::Agogic& agogic) {
        if (measure == measures.end()) {
            time_signature = seq::TimeSignature();
            dynamics = seq::Dynamics();
            agogic = seq::Agogic();
            return;
        }

        time_signature = measure->time_signature;

        switch (measure->dynamics.index()) {
            case 0:
                dynamics = seq::ConstantLoudness {std::get<0>(measure->dynamics).loudness};
                break;
            case 1:
                dynamics = seq::ConstantLoudness {std::get<1>(measure->dynamics).loudness_end};
                break;
        }

        switch (measure->agogic.index()) {
            case 0:
                agogic = seq::ConstantTempo {std::get<0>(measure->agogic).tempo};
                break;
            case 1:
                agogic = seq::ConstantTempo {std::get<1>(measure->agogic).tempo_end};
                break;
        }
    }

    void Application::set_dynamics(seq::Measure& measure, ui::Dynamics dynamics) {
        if (dynamics.varying) {
            measure.dynamics = seq::VaryingLoudness {seq::Loudness(dynamics.loudness1), seq::Loudness(dynamics.loudness2)};
        } else {
            measure.dynamics = seq::ConstantLoudness {seq::Loudness(dynamics.loudness1)};
        }
    }

    void Application::set_dynamics(ui::Dynamics& dynamics, const seq::Measure& measure) {
        switch (measure.dynamics.index()) {
            case 0:
                dynamics.varying = false;
                dynamics.loudness1 = ui::Loudness(std::get<0>(measure.dynamics).loudness);
                break;
            case 1:
                dynamics.varying = true;
                dynamics.loudness1 = ui::Loudness(std::get<1>(measure.dynamics).loudness_begin);
                dynamics.loudness2 = ui::Loudness(std::get<1>(measure.dynamics).loudness_end);
                break;
        }
    }

    void Application::set_agogic(seq::Measure& measure, ui::Agogic agogic) {
        if (agogic.varying) {
            measure.agogic = seq::VaryingTempo {seq::Tempo(agogic.tempo1), seq::Tempo(agogic.tempo2)};
        } else {
            measure.agogic = seq::ConstantTempo {seq::Tempo(agogic.tempo1)};
        }
    }

    void Application::set_agogic(ui::Agogic& agogic, const seq::Measure& measure) {
        switch (measure.agogic.index()) {
            case 0:
                agogic.varying = false;
                agogic.tempo1 = ui::Tempo(std::get<0>(measure.agogic).tempo);
                break;
            case 1:
                agogic.varying = true;
                agogic.tempo1 = ui::Tempo(std::get<1>(measure.agogic).tempo_begin);
                agogic.tempo2 = ui::Tempo(std::get<1>(measure.agogic).tempo_end);
                break;
        }
    }

    void Application::set_time_signature(seq::Measure& measure, ui::TimeSignature time_signature) {
        seq::Beats beats {};
        seq::Value value {};

        switch (time_signature.beats) {
            case ui::TimeSignature::Beats1: beats = 1; break;
            case ui::TimeSignature::Beats2: beats = 2; break;
            case ui::TimeSignature::Beats3: beats = 3; break;
            case ui::TimeSignature::Beats4: beats = 4; break;
            case ui::TimeSignature::Beats5: beats = 5; break;
            case ui::TimeSignature::Beats6: beats = 6; break;
            case ui::TimeSignature::Beats7: beats = 7; break;
            case ui::TimeSignature::Beats8: beats = 8; break;
            case ui::TimeSignature::Beats9: beats = 9; break;
            case ui::TimeSignature::Beats10: beats = 10; break;
            case ui::TimeSignature::Beats11: beats = 11; break;
            case ui::TimeSignature::Beats12: beats = 12; break;
            case ui::TimeSignature::Beats13: beats = 13; break;
            case ui::TimeSignature::Beats14: beats = 14; break;
            case ui::TimeSignature::Beats15: beats = 15; break;
            case ui::TimeSignature::Beats16: beats = 16; break;
        }

        switch (time_signature.value) {
            case ui::TimeSignature::Value1: value = seq::Whole; break;
            case ui::TimeSignature::Value2: value = seq::Half; break;
            case ui::TimeSignature::Value4: value = seq::Quarter; break;
            case ui::TimeSignature::Value8: value = seq::Eighth; break;
            case ui::TimeSignature::Value16: value = seq::Sixteenth; break;
        }

        measure.time_signature = seq::TimeSignature(beats, value);
    }

    void Application::set_time_signature(ui::TimeSignature& time_signature, const seq::Measure& measure) {
        switch (measure.time_signature.beats()) {
            case 1: time_signature.beats = ui::TimeSignature::Beats1; break;
            case 2: time_signature.beats = ui::TimeSignature::Beats2; break;
            case 3: time_signature.beats = ui::TimeSignature::Beats3; break;
            case 4: time_signature.beats = ui::TimeSignature::Beats4; break;
            case 5: time_signature.beats = ui::TimeSignature::Beats5; break;
            case 6: time_signature.beats = ui::TimeSignature::Beats6; break;
            case 7: time_signature.beats = ui::TimeSignature::Beats7; break;
            case 8: time_signature.beats = ui::TimeSignature::Beats8; break;
            case 9: time_signature.beats = ui::TimeSignature::Beats9; break;
            case 10: time_signature.beats = ui::TimeSignature::Beats10; break;
            case 11: time_signature.beats = ui::TimeSignature::Beats11; break;
            case 12: time_signature.beats = ui::TimeSignature::Beats12; break;
            case 13: time_signature.beats = ui::TimeSignature::Beats13; break;
            case 14: time_signature.beats = ui::TimeSignature::Beats14; break;
            case 15: time_signature.beats = ui::TimeSignature::Beats15; break;
            case 16: time_signature.beats = ui::TimeSignature::Beats16; break;
        }

        switch (measure.time_signature.value()) {
            case seq::Whole: time_signature.value = ui::TimeSignature::Value1; break;
            case seq::Half: time_signature.value = ui::TimeSignature::Value2; break;
            case seq::Quarter: time_signature.value = ui::TimeSignature::Value4; break;
            case seq::Eighth: time_signature.value = ui::TimeSignature::Value8; break;
            case seq::Sixteenth: time_signature.value = ui::TimeSignature::Value16; break;
        }
    }

    bool Application::measure_empty(const seq::Measure& measure) {
        return std::ranges::all_of(measure.instruments, [](const auto& instrument) {
            return instrument.second.empty();
        });
    }

    bool Application::check_note_up_limit(const seq::Note& note) {
        return note.id < std::uint32_t(syn::keyboard::NOTES - 1);
    }

    bool Application::check_note_down_limit(const seq::Note& note) {
        return note.id > 0;
    }

    bool Application::check_note_left_limit(const seq::Note& note) {
        return note.position > 0;
    }

    bool Application::check_note_right_limit(const seq::Note& note, const seq::Measure& measure) {
        return note.position < measure.time_signature.measure_steps() - seq::steps(note.value, note.tuplet);
    }

    bool Application::notes_overlapping(const seq::Note& note1, const seq::Note& note2) {
        if (note1.id != note2.id) {
            return false;
        }

        const auto note1_left = note1.position;
        const auto note1_right = note1.position + seq::steps(note1.value, note1.tuplet);
        const auto note2_left = note2.position;
        const auto note2_right = note2.position + seq::steps(note2.value, note2.tuplet);

        return
            note1_left > note2_left && note1_left < note2_right ||
            note1_right > note2_left && note1_right < note2_right ||
            note2_left > note1_left && note2_left < note1_right ||
            note2_right > note1_left && note2_right < note1_right;
    }

    bool Application::note_in_selection(NoteIter note, MeasureIter measure, const std::vector<ProvenanceNote>& selected_notes) {
        return std::ranges::find_if(selected_notes, [note, measure](const auto& n) {
            return measure == n.measure() && note == n.note();
        }) != selected_notes.end();
    }

    std::optional<ProvenanceNote> Application::check_note_has_next(const ProvenanceNote& provenance_note) const {
        return m_composition.check_note_has_next(provenance_note);
    }

    std::optional<ProvenanceNote> Application::check_note_has_previous(const ProvenanceNote& provenance_note) const {
        return m_composition.check_note_has_previous(provenance_note);
    }

    Time Application::elapsed_seconds_to_time(double elapsed_seconds) {
        Time time;

        double total_seconds {};
        const double fraction_seconds = std::modf(elapsed_seconds, &total_seconds);

        const std::div_t division = std::div(int(total_seconds), 60);

        time.minutes = division.quot;
        time.seconds = division.rem;
        time.deciseconds = int(fraction_seconds * 10.0);

        return time;
    }

    seq::Value Application::translate_value(ui::Value value) {
        switch (value) {
            case ui::ValueWhole:
                return seq::Whole;
            case ui::ValueHalf:
                return seq::Half;
            case ui::ValueQuarter:
                return seq::Quarter;
            case ui::ValueEighth:
                return seq::Eighth;
            case ui::ValueSixteenth:
                return seq::Sixteenth;
        }

        std::unreachable();
    }

    preset::Envelope Application::translate_envelope(const ui::preset::Envelope& envelope) {
        preset::Envelope result_envelope;

        switch (envelope.type) {
            case ui::preset::EnvelopeTypeAdsrLinear:
            case ui::preset::EnvelopeTypeAdsrExponential:
                result_envelope.description = syn::envelope::DescriptionAdsr {
                    .duration_attack = envelope.description.duration_attack,
                    .duration_decay = envelope.description.duration_decay,
                    .duration_release = envelope.description.duration_release,
                    .value_sustain = envelope.description.value_sustain
                };
                break;
            case ui::preset::EnvelopeTypeAdrLinear:
            case ui::preset::EnvelopeTypeAdrExponential:
                result_envelope.description = syn::envelope::DescriptionAdr {
                    .duration_attack = envelope.description.duration_attack,
                    .duration_decay = envelope.description.duration_decay,
                    .duration_release = envelope.description.duration_release
                };
                break;
            case ui::preset::EnvelopeTypeNull:
                result_envelope.description = syn::envelope::DescriptionNull();
                break;
        }

        switch (envelope.type) {
            case ui::preset::EnvelopeTypeAdsrLinear:
            case ui::preset::EnvelopeTypeAdrLinear:
                result_envelope.type = syn::envelope::Type::Linear;
                break;
            case ui::preset::EnvelopeTypeAdsrExponential:
            case ui::preset::EnvelopeTypeAdrExponential:
                result_envelope.type = syn::envelope::Type::Exponential;
                break;
            case ui::preset::EnvelopeTypeNull:
                break;
        }

        return result_envelope;
    }

    ui::preset::Envelope Application::translate_envelope(const preset::Envelope& envelope) {
        ui::preset::Envelope result_envelope;

        switch (envelope.description.index()) {
            case 0:
                result_envelope.description.duration_attack = std::get<0>(envelope.description).duration_attack;
                result_envelope.description.duration_decay = std::get<0>(envelope.description).duration_decay;
                result_envelope.description.duration_release = std::get<0>(envelope.description).duration_release;
                result_envelope.description.value_sustain = std::get<0>(envelope.description).value_sustain;

                switch (envelope.type) {
                    case syn::envelope::Type::Linear:
                        result_envelope.type = ui::preset::EnvelopeTypeAdsrLinear;
                        break;
                    case syn::envelope::Type::Exponential:
                        result_envelope.type = ui::preset::EnvelopeTypeAdsrExponential;
                        break;
                }

                break;
            case 1:
                result_envelope.description.duration_attack = std::get<1>(envelope.description).duration_attack;
                result_envelope.description.duration_decay = std::get<1>(envelope.description).duration_decay;
                result_envelope.description.duration_release = std::get<1>(envelope.description).duration_release;

                switch (envelope.type) {
                    case syn::envelope::Type::Linear:
                        result_envelope.type = ui::preset::EnvelopeTypeAdrLinear;
                        break;
                    case syn::envelope::Type::Exponential:
                        result_envelope.type = ui::preset::EnvelopeTypeAdrExponential;
                        break;
                }

                break;
            case 2:
                result_envelope.type = ui::preset::EnvelopeTypeNull;

                break;
        }

        return result_envelope;
    }

    preset::BasePreset Application::translate_preset(const ui::preset::BasePreset& preset) {
        preset::BasePreset result_preset;

        result_preset.name = preset.name;
        result_preset.description = preset.description;
        result_preset.range = std::make_pair(preset.range[0], preset.range[1]);
        result_preset.envelope = translate_envelope(preset.envelope);

        return result_preset;
    }

    ui::preset::BasePreset Application::translate_preset(const preset::BasePreset& preset) {
        ui::preset::BasePreset result_preset;

        std::strncpy(result_preset.name, preset.name.c_str(), sizeof(result_preset.name) - 1);
        std::strncpy(result_preset.description, preset.description.c_str(), sizeof(result_preset.description) - 1);
        result_preset.range[0] = preset.range.first;
        result_preset.range[1] = preset.range.second;
        result_preset.envelope = translate_envelope(preset.envelope);

        return result_preset;
    }

    preset::add::Preset Application::translate_preset(const ui::preset::PresetAdd& preset) {
        preset::add::Preset result_preset;
        static_cast<preset::BasePreset&>(result_preset) = translate_preset(static_cast<const ui::preset::BasePreset&>(preset));

        for (const ui::preset::Partial& partial : preset.partials) {
            result_preset.partials.emplace_back(
                syn::oscillator::Type(partial.oscillator_type),
                partial.frequency_multiplier,
                partial.amplitude_divisor,
                partial.phase,
                partial.lfo.enabled
                    ?
                    std::make_optional(syn::LowFrequencyOscillator { .frequency = partial.lfo.frequency, .deviation = partial.lfo.deviation })
                    :
                    std::nullopt,
                translate_envelope(partial.envelope)
            );
        }

        return result_preset;
    }

    ui::preset::PresetAdd Application::translate_preset(const preset::add::Preset& preset) {
        ui::preset::PresetAdd result_preset;
        static_cast<ui::preset::BasePreset&>(result_preset) = translate_preset(static_cast<const preset::BasePreset&>(preset));

        for (const preset::add::Partial& partial : preset.partials) {
            ui::preset::Partial& result_partial = result_preset.partials.emplace_back();

            result_partial.oscillator_type = ui::preset::OscillatorType(partial.oscillator_type);
            result_partial.frequency_multiplier = partial.frequency_multiplier;
            result_partial.amplitude_divisor = partial.amplitude_divisor;
            result_partial.phase = partial.phase;

            if (!partial.lfo) {
                result_partial.lfo.enabled = false;
            } else {
                result_partial.lfo.enabled = true;
                result_partial.lfo.frequency = partial.lfo->frequency;
                result_partial.lfo.deviation = partial.lfo->deviation;
            }

            result_partial.envelope = translate_envelope(partial.envelope);
        }

        return result_preset;
    }

    preset::pad::Preset Application::translate_preset(const ui::preset::PresetPad& preset) {
        preset::pad::Preset result_preset;
        static_cast<preset::BasePreset&>(result_preset) = translate_preset(static_cast<const ui::preset::BasePreset&>(preset));

        switch (preset.profile) {
            case ui::preset::ProfileGaussian:
                result_preset.profile = preset::pad::Profile::Gaussian;
                break;
            case ui::preset::ProfileEven:
                result_preset.profile = preset::pad::Profile::Even;
                break;
            case ui::preset::ProfileSingleSine:
                result_preset.profile = preset::pad::Profile::SingleSine;
                break;
            case ui::preset::ProfileDoubleSine:
                result_preset.profile = preset::pad::Profile::DoubleSine;
                break;
            case ui::preset::ProfileArc:
                result_preset.profile = preset::pad::Profile::Arc;
                break;
        }

        result_preset.frequency = preset.frequency;
        result_preset.bandwidth = preset.bandwidth;
        result_preset.amplitude_harmonics = preset.amplitude_harmonics;

        return result_preset;
    }

    ui::preset::PresetPad Application::translate_preset(const preset::pad::Preset& preset) {
        ui::preset::PresetPad result_preset;
        static_cast<ui::preset::BasePreset&>(result_preset) = translate_preset(static_cast<const preset::BasePreset&>(preset));

        switch (preset.profile) {
            case preset::pad::Profile::Gaussian:
                result_preset.profile = ui::preset::ProfileGaussian;
                break;
            case preset::pad::Profile::Even:
                result_preset.profile = ui::preset::ProfileEven;
                break;
            case preset::pad::Profile::SingleSine:
                result_preset.profile = ui::preset::ProfileSingleSine;
                break;
            case preset::pad::Profile::DoubleSine:
                result_preset.profile = ui::preset::ProfileDoubleSine;
                break;
            case preset::pad::Profile::Arc:
                result_preset.profile = ui::preset::ProfileArc;
                break;
        }

        result_preset.frequency = preset.frequency;
        result_preset.bandwidth = preset.bandwidth;
        result_preset.amplitude_harmonics = preset.amplitude_harmonics;

        return result_preset;
    }

    const ImVec4& Application::color(ImGuiCol color) {
        return ImGui::GetStyle().Colors[color];
    }

    ImColor Application::color_opacity(ImGuiCol color, float opacity) {
        ImColor color_ = ImGui::GetStyle().Colors[color];
        color_.Value.w = opacity;
        return color_;
    }

    void Application::check_path_extension(const std::filesystem::path& path, const char* extension) {
        if (path.extension() != extension) {
            LOG_WARNING("File has the wrong extension: {}", path.extension().string());
        }
    }

    void Application::save_file_dialog(void* userdata, const char* const* filelist, int) {
        Application& self = *static_cast<FileDialogData*>(userdata)->self;
        FileDialogData::Function function = static_cast<FileDialogData*>(userdata)->function;

        delete static_cast<FileDialogData*>(userdata);

        if (!filelist) {
            self.m_task_manager.add_immediate_thread_safe_task([&self, error = std::string(SDL_GetError())] {
                logging::error("An error occurred while handling the save file dialog: {}", error);
                self.notify_message("An error occurred while handling the save file dialog");
            });

            return;
        }

        if (const char* file = filelist[0]; file) {
            self.m_task_manager.add_immediate_thread_safe_task([&self, function, file = std::string(file)] mutable {
                (void) (self.*function)(std::move(file));
            });
        }
    }

    void Application::open_file_dialog(void* userdata, const char* const* filelist, int) {
        Application& self = *static_cast<FileDialogData*>(userdata)->self;
        FileDialogData::Function function = static_cast<FileDialogData*>(userdata)->function;

        delete static_cast<FileDialogData*>(userdata);

        if (!filelist) {
            self.m_task_manager.add_immediate_thread_safe_task([&self, error = std::string(SDL_GetError())] {
                logging::error("An error occurred while handling the open file dialog: {}", error);
                self.notify_message("An error occurred while handling the open file dialog");
            });

            return;
        }

        if (const char* file = filelist[0]; file) {
            self.m_task_manager.add_immediate_thread_safe_task([&self, function, file = std::string(file)] mutable {
                (void) (self.*function)(std::move(file));
            });
        }
    }

    void Application::composition_write(const std::filesystem::path& path, const composition::Composition& composition) {
        utility::Buffer buffer;
        composition::export_composition(composition, buffer);
        utility::write_file(path, buffer);
    }

    void Application::composition_read(const std::filesystem::path& path, composition::Composition& composition) {
        utility::Buffer buffer;
        utility::read_file(path, buffer);
        composition::import_composition(composition, buffer);
    }

    bool Application::composition_save(std::filesystem::path path) {
        path.replace_extension(".alfred");

        strip_composition_empty_instruments(m_composition);

        try {
            composition_write(path, m_composition);
        } catch (const composition::CompositionError& e) {
            logging::error("Could not save composition: {}", e.what());
            notify_message(std::format("Could not save composition: {}", e.name()));
            return false;
        } catch (const utility::FileError& e) {
            logging::error("Could not save composition: {}", e.what());
            notify_message(std::format("Could not save composition: {}", e.name()));
            return false;
        }

        logging::information("Written composition to `{}`", path.string());
        notify_message("Written composition to the specified path");

        m_composition_path = std::move(path);
        m_composition_not_saved = false;
        m_data.recent_compositions.insert(m_composition_path.string());

        set_title_composition_saved();

        return true;
    }

    bool Application::composition_save() {
        assert(!m_composition_path.empty());

        check_path_extension(m_composition_path, ".alfred");
        strip_composition_empty_instruments(m_composition);

        try {
            composition_write(m_composition_path, m_composition);
        } catch (const composition::CompositionError& e) {
            logging::error("Could not save composition: {}", e.what());
            notify_message(std::format("Could not save composition: {}", e.name()));
            return false;
        } catch (const utility::FileError& e) {
            logging::error("Could not save composition: {}", e.what());
            notify_message(std::format("Could not save composition: {}", e.name()));
            return false;
        }

        logging::information("Written composition to `{}`", m_composition_path.string());
        notify_message("Written composition to the specified path");

        m_composition_not_saved = false;
        m_data.recent_compositions.insert(m_composition_path.string());

        set_title_composition_saved();

        return true;
    }

    bool Application::composition_open(std::filesystem::path path) {
        check_path_extension(path, ".alfred");

        try {
            composition_read(path, m_composition);
        } catch (const composition::CompositionError& e) {
            m_composition = {};
            logging::error("Could not open composition: {}", e.what());
            notify_message(std::format("Could not open composition: {}", e.name()));
            return false;
        } catch (const utility::FileError& e) {
            m_composition = {};
            logging::error("Could not open composition: {}", e.what());
            notify_message(std::format("Could not open composition: {}", e.name()));
            return false;
        }

        logging::information("Read composition from `{}`", path.string());
        notify_message("Read composition from the specified path");

        m_composition_path = std::move(path);
        m_data.recent_compositions.insert(m_composition_path.string());

        std::strncpy(m_ui.composition.title, m_composition.title.c_str(), sizeof(m_ui.composition.title) - 1);
        std::strncpy(m_ui.composition.author, m_composition.author.c_str(), sizeof(m_ui.composition.author) - 1);
        m_ui.composition.year = short(int(m_composition.year));

        reset_everything();
        reset_composition_flags();
        invalidate_composition();
        set_title_composition_saved();
        set_synthesizer_instrument_volumes(m_synthesizer);
        set_composition_instrument_colors();

        return true;
    }

    void Application::composition_new() {
        m_composition_path.clear();
        m_composition = {};
        m_ui.composition = {};

        reset_everything();
        reset_composition_flags();
        set_title_composition_saved();
        reset_synthesizer_instrument_volumes(m_synthesizer);
        reset_composition_instrument_colors();
    }

    void Application::composition_file_save() {
        constexpr SDL_DialogFileFilter filters[] {
            { "Alfred files", "alfred" },
            { "All files", "*" }
        };

        if (m_composition_path.empty()) {
            SDL_ShowSaveFileDialog(
                &Application::save_file_dialog,
                new FileDialogData { .self = this, .function = &Application::composition_save },
                m_window,
                filters,
                std::size(filters),
                nullptr
            );
        } else {
            (void) composition_save();
        }
    }

    void Application::composition_file_open() {
        constexpr SDL_DialogFileFilter filters[] {
            { "Alfred files", "alfred" },
            { "All files", "*" }
        };

        SDL_ShowOpenFileDialog(
            &Application::open_file_dialog,
            new FileDialogData { .self = this, .function = &Application::composition_open },
            m_window,
            filters,
            std::size(filters),
            nullptr,
            false
        );
    }

    void Application::composition_file_new() {
        composition_new();
    }

    void Application::preset_write(const std::filesystem::path& path, const preset::add::Preset& preset) {
        utility::Buffer buffer;
        preset::export_preset(preset, buffer);
        utility::write_file(path, buffer);
    }

    void Application::preset_write(const std::filesystem::path& path, const preset::pad::Preset& preset) {
        utility::Buffer buffer;
        preset::export_preset(preset, buffer);
        utility::write_file(path, buffer);
    }

    void Application::preset_read(const std::filesystem::path& path, preset::add::Preset& preset) {
        utility::Buffer buffer;
        utility::read_file(path, buffer);
        preset::import_preset(preset, buffer);
    }

    void Application::preset_read(const std::filesystem::path& path, preset::pad::Preset& preset) {
        utility::Buffer buffer;
        utility::read_file(path, buffer);
        preset::import_preset(preset, buffer);
    }

    bool Application::preset_save_add(std::filesystem::path path) {
        path.replace_extension(".addpreset");

        try {
            preset_write(path, translate_preset(m_ui.preset_add));
        } catch (const preset::PresetError& e) {
            logging::error("Could not save preset: {}", e.what());
            notify_message(std::format("Could not save preset: {}", e.name()));
            return false;
        } catch (const utility::FileError& e) {
            logging::error("Could not save preset: {}", e.what());
            notify_message(std::format("Could not save preset: {}", e.name()));
            return false;
        }

        logging::information("Written preset to `{}`", path.string());
        notify_message("Written preset to the specified path");

        return true;
    }

    bool Application::preset_save_pad(std::filesystem::path path) {
        path.replace_extension(".padpreset");

        try {
            preset_write(path, translate_preset(m_ui.preset_pad));
        } catch (const preset::PresetError& e) {
            logging::error("Could not save preset: {}", e.what());
            notify_message(std::format("Could not save preset: {}", e.name()));
            return false;
        } catch (const utility::FileError& e) {
            logging::error("Could not save preset: {}", e.what());
            notify_message(std::format("Could not save preset: {}", e.name()));
            return false;
        }

        logging::information("Written preset to `{}`", path.string());
        notify_message("Written preset to the specified path");

        return true;
    }

    bool Application::preset_open_add(std::filesystem::path path) {
        if (!preset_open_add_silent(std::move(path), m_ui.preset_add)) {
            return false;
        }

        logging::information("Read preset from `{}`", path.string());
        notify_message("Read preset from the specified path");

        open_create_instrument(ui::CreateInstrumentTab::Additive);

        return true;
    }

    bool Application::preset_open_pad(std::filesystem::path path) {
        if (!preset_open_pad_silent(std::move(path), m_ui.preset_pad)) {
            return false;
        }

        logging::information("Read preset from `{}`", path.string());
        notify_message("Read preset from the specified path");

        open_create_instrument(ui::CreateInstrumentTab::PadSynth);

        return true;
    }

    bool Application::preset_open_add_silent(std::filesystem::path path, ui::preset::PresetAdd& preset_add) const {
        check_path_extension(path, ".addpreset");

        preset::add::Preset preset;

        try {
            preset_read(path, preset);
        } catch (const preset::PresetError& e) {
            logging::error("Could not open preset: {}", e.what());
            notify_message(std::format("Could not open preset: {}", e.name()));
            return false;
        } catch (const utility::FileError& e) {
            logging::error("Could not open preset: {}", e.what());
            notify_message(std::format("Could not open preset: {}", e.name()));
            return false;
        }

        preset_add = translate_preset(preset);

        return true;
    }

    bool Application::preset_open_pad_silent(std::filesystem::path path, ui::preset::PresetPad& preset_pad) const {
        check_path_extension(path, ".padpreset");

        preset::pad::Preset preset;

        try {
            preset_read(path, preset);
        } catch (const preset::PresetError& e) {
            logging::error("Could not open preset: {}", e.what());
            notify_message(std::format("Could not open preset: {}", e.name()));
            return false;
        } catch (const utility::FileError& e) {
            logging::error("Could not open preset: {}", e.what());
            notify_message(std::format("Could not open preset: {}", e.name()));
            return false;
        }

        preset_pad = translate_preset(preset);

        return true;
    }

    void Application::preset_file_save_add() {
        constexpr SDL_DialogFileFilter filters[] {
            { "Additive preset files", "addpreset" },
            { "All files", "*" }
        };

        SDL_ShowSaveFileDialog(
            &Application::save_file_dialog,
            new FileDialogData { .self = this, .function = &Application::preset_save_add },
            m_window,
            filters,
            std::size(filters),
            nullptr
        );
    }

    void Application::preset_file_save_pad() {
        constexpr SDL_DialogFileFilter filters[] {
            { "PadSynth preset files", "padpreset" },
            { "All files", "*" }
        };

        SDL_ShowSaveFileDialog(
            &Application::save_file_dialog,
            new FileDialogData { .self = this, .function = &Application::preset_save_pad },
            m_window,
            filters,
            std::size(filters),
            nullptr
        );
    }

    void Application::preset_file_open_add() {
        constexpr SDL_DialogFileFilter filters[] {
            { "Additive preset files", "addpreset" },
            { "All files", "*" }
        };

        SDL_ShowOpenFileDialog(
            &Application::open_file_dialog,
            new FileDialogData { .self = this, .function = &Application::preset_open_add },
            m_window,
            filters,
            std::size(filters),
            nullptr,
            false
        );
    }

    void Application::preset_file_open_pad() {
        constexpr SDL_DialogFileFilter filters[] {
            { "PadSynth preset files", "padpreset" },
            { "All files", "*" }
        };

        SDL_ShowOpenFileDialog(
            &Application::open_file_dialog,
            new FileDialogData { .self = this, .function = &Application::preset_open_pad },
            m_window,
            filters,
            std::size(filters),
            nullptr,
            false
        );
    }

    void Application::open_composition_metadata() {
        m_composition_metadata_menu = true;
    }

    void Application::open_composition_mixer() {
        m_composition_mixer_menu = true;
    }

    void Application::open_create_instrument(std::optional<ui::CreateInstrumentTab> create_instrument_tab) {
        m_create_instrument_menu = true;
        m_ui.create_instrument_tab_select = create_instrument_tab;
    }

    void Application::open_render_composition() {
        if (!m_render_in_progress) {
            reset_render_composition();
        }

        m_render_composition_menu = true;
    }

    void Application::load_presets_from_disk() {
        std::vector<std::filesystem::path> paths;

        try {
            paths = utility::glob_directory(utility::data_file_path() / PRESETS_DIRECTORY, "*.???preset");
        } catch (const utility::FileError& e) {
            logging::error("Could not glob presets directory: {}", e.what());
            notify_message(std::format("Could not glob presets directory: {}", e.name()));
            return;
        }

        if (paths.size() == 1) {
            logging::information("Found 1 preset on disk");
            notify_message("Found 1 preset on disk");
        } else {
            logging::information("Found {} presets on disk", paths.size());
            notify_message(std::format("Found {} presets on disk", paths.size()));
        }

        for (const auto& path : paths) {
            if (path.extension() == ".addpreset") {
                ui::preset::PresetAdd preset;
                preset_open_add_silent(utility::data_file_path() / PRESETS_DIRECTORY / path, preset);

                (void) m_synthesizer.store_instrument(std::make_unique<preset::add::RuntimeInstrument>(translate_preset(preset)));
                set_composition_instrument_colors();
            } else if (path.extension() == ".padpreset") {
                ui::preset::PresetPad preset;
                preset_open_pad_silent(utility::data_file_path() / PRESETS_DIRECTORY / path, preset);

                (void) m_synthesizer.store_instrument(std::make_unique<preset::pad::RuntimeInstrument>(translate_preset(preset)));
                set_composition_instrument_colors();
            } else {
                LOG_WARNING("Unknown preset file: {}", path.string());
            }
        }
    }

    void Application::reset_render_composition() {
        if (!m_composition_path.empty()) {
            std::strncpy(m_ui.render_file_path, std::filesystem::path(m_composition_path).replace_extension().string().c_str(), sizeof(m_ui.render_file_path) - 1);
        } else {
            std::strncpy(m_ui.render_file_path, (std::filesystem::path(m_working_directory) / "unsaved_composition").string().c_str(), sizeof(m_ui.render_file_path) - 1);
        }

        m_ui.render_normalize = true;
        m_ui.render_progress = 0.0f;
    }

    void Application::start_render_composition() {
        assert(*m_ui.render_file_path != 0);

        m_render_in_progress = true;

        // Create the synthesizer here and set up all the parameters here before in the main thread, not during the async task, in the other thread
        // We must copy all the data in this instant and work with them later whenever the other thread starts
        synthesizer::VirtualSynthesizer synthesizer;
        synthesizer.merge_instruments(m_synthesizer);

        m_task_manager.add_async_task([
            this,
            synthesizer = std::move(synthesizer),
            path = std::filesystem::path(m_ui.render_file_path),
            composition = m_composition,
            normalize = m_ui.render_normalize,
            render_progress = &m_ui.render_progress
        ](task::AsyncTask& task) mutable {
            path.replace_extension(".wav");

            try {
                RenderCompositionParameters parameters;
                parameters.synthesizer = std::move(synthesizer);
                parameters.file_path = std::move(path);
                parameters.composition = std::move(composition);
                parameters.normalize = normalize;
                parameters.render_progress = render_progress;

                do_render_composition(task, m_task_manager, [this](std::string message) { notify_message(std::move(message)); }, std::move(parameters));
            } catch (const seq::SequencerError& e) {
                logging::error("Error rendering composition: {}", e.what());
                notify_message(std::format("Error rendering composition: {}", e.name()));
            } catch (const encoder::EncoderError& e) {
                logging::error("Error rendering composition: {}", e.what());
                notify_message(std::format("Error rendering composition: {}", e.name()));
            } catch (const utility::FileError& e) {
                logging::error("Error rendering composition: {}", e.what());
                notify_message(std::format("Error rendering composition: {}", e.name()));
            } catch (...) {
                m_task_manager.add_immediate_thread_safe_task([this] {
                    m_render_in_progress = false;
                });

                task.finish(std::current_exception());

                return;
            }

            m_task_manager.add_immediate_thread_safe_task([this] {
                m_render_in_progress = false;
            });

            task.finish();
        });
    }

    void Application::do_render_composition(const task::AsyncTask& task, task::TaskManager& task_manager, const NotifyMessage& notify_message, RenderCompositionParameters parameters) {
        using namespace std::chrono_literals;
        using TimePoint = std::chrono::time_point<std::chrono::system_clock>;

        bool rendering = true;

        seq::Player player {parameters.synthesizer, parameters.composition, [&rendering] { rendering = false; }};

        parameters.synthesizer.polyphony(optimal_composition_voices(parameters.composition));
        set_synthesizer_instrument_volumes(parameters.synthesizer, parameters.composition);

        logging::debug("Starting rendering composition to `{}`", parameters.file_path.string());

        player.start();

        const TimePoint time_start = std::chrono::system_clock::now();
        TimePoint time_last_update = time_start;

        for (int i {}; i < def::SAMPLE_FREQUENCY / 10; i++) {
            parameters.synthesizer.update();
        }

        while (rendering) {
            player.update(1.0 / double(def::SAMPLE_FREQUENCY));
            (void) parameters.synthesizer.update();

            assert(player.in_time());

            if (task.stop_requested()) {
                logging::information("Interrupted rendering composition");
                notify_message("Interrupted rendering composition");
                return;
            }

            const TimePoint time_now = std::chrono::system_clock::now();

            if (time_now - time_last_update > 0.1s) {
                time_last_update = time_now;

                task_manager.add_immediate_thread_safe_task([render_progress = parameters.render_progress, position = player.position(), size = parameters.composition.size()] {
                    *render_progress = math::map(float(position), 0.0f, float(size), 0.0f, 0.95f);
                });
            }
        }

        for (int i {}; i < def::SAMPLE_FREQUENCY / 2; i++) {
            parameters.synthesizer.update();
        }

        const TimePoint time_stop = std::chrono::system_clock::now();

        if (parameters.normalize) {
            LOG_INFORMATION("Normalizing composition");
            math::normalize(parameters.synthesizer.get_buffer_data(), parameters.synthesizer.get_buffer_size());
        }

        utility::write_file(parameters.file_path, encoder::encode_wav(parameters.synthesizer.get_buffer_size(), parameters.synthesizer.get_buffer_data()));

        task_manager.add_immediate_thread_safe_task([render_progress = parameters.render_progress] {
            *render_progress = 1.0f;
        });

        const auto duration = std::chrono::duration_cast<std::chrono::seconds>(time_stop - time_start);

        logging::information("Done rendering composition in {}", duration);
        notify_message(std::format("Done rendering composition in {}", duration));
    }

    std::size_t Application::max_composition_voices(const seq::Composition& composition) {
        if (composition.size() == 0) {
            return 0;
        }

        auto time_line = std::make_unique<std::size_t[]>(composition.size());
        std::uint32_t steps {};

        for (const seq::Measure& measure : composition.measures) {
            for (const auto& notes: measure.instruments | std::views::values) {
                for (const seq::Note& note : notes) {
                    for (std::uint32_t i {}; i < seq::steps(note.value, note.tuplet); i++) {
                        time_line[steps + note.position + i]++;
                    }
                }
            }

            steps += measure.time_signature.measure_steps();
        }

        return *std::max_element(time_line.get(), time_line.get() + composition.size());
    }

    std::size_t Application::optimal_composition_voices(const seq::Composition& composition) {
        return std::size_t(std::round(double(max_composition_voices(composition)) * 1.5));
    }

    void Application::strip_composition_empty_instruments(seq::Composition& composition) {
        for (seq::Measure& measure : composition.measures) {
            for (auto instrument = measure.instruments.begin(); instrument != measure.instruments.end();) {
                if (instrument->second.empty()) {
                    instrument = measure.instruments.erase(instrument);
                    continue;
                }

                instrument++;
            }
        }
    }

    void Application::undo() {
        assert(!m_composition_history.undo.empty());

        seq::Composition& composition = m_composition;

        m_composition_history.redo.emplace_back(std::move(composition), m_composition_camera);

        composition = std::move(m_composition_history.undo.back().composition);
        m_composition_camera = m_composition_history.undo.back().camera;

        m_composition_history.undo.pop_back();

        modify_composition();
        reset_composition_selection();
        keep_player_cursor_valid();
    }

    void Application::redo() {
        assert(!m_composition_history.redo.empty());

        seq::Composition& composition = m_composition;

        m_composition_history.undo.emplace_back(std::move(composition), m_composition_camera);

        composition = std::move(m_composition_history.redo.back().composition);
        m_composition_camera = m_composition_history.redo.back().camera;

        m_composition_history.redo.pop_back();

        modify_composition();
        reset_composition_selection();
        keep_player_cursor_valid();
    }

    void Application::remember_composition() {
        seq::Composition& composition = m_composition;

        m_composition_history.undo.emplace_back(composition, m_composition_camera);
        m_composition_history.redo.clear();

        // Set a limit to the stack size
        // Erase multiple elements occasionally, instead of erasing for every change
        if (m_composition_history.undo.size() > 90) {
            m_composition_history.undo.erase(m_composition_history.undo.begin(), std::next(m_composition_history.undo.begin(), 30));
        }
    }

    void Application::keep_player_cursor_valid() {
        if (m_player.position() > m_composition.size()) {
            m_player.seek(m_composition.size());
        }
    }

    void Application::set_synthesizer_instrument_volumes(synthesizer::Synthesizer& synthesizer) const {
        set_synthesizer_instrument_volumes(synthesizer, m_composition);
    }

    void Application::set_synthesizer_instrument_volumes(synthesizer::Synthesizer& synthesizer, const composition::Composition& composition) {
        reset_synthesizer_instrument_volumes(synthesizer);

        for (const auto& [instrument, volume] : composition.instrument_volumes) {
            synthesizer.mixer_volume(instrument, volume);
        }
    }

    void Application::reset_synthesizer_instrument_volumes(synthesizer::Synthesizer& synthesizer) {
        synthesizer.mixer_reset();
    }

    void Application::set_composition_instrument_colors() {
        reset_composition_instrument_colors();

        for (const auto& [instrument, color] : m_composition.instrument_colors) {
            m_ui.colors[instrument] = color;
        }
    }

    void Application::reset_composition_instrument_colors() {
        m_synthesizer.for_each_instrument([this, index = std::size_t()](const auto& instrument) mutable {
            m_ui.colors[instrument.id()] = ui::ColorIndex(index);
            index = (index + 1) % std::size(ui::COLORS);
        });
    }

    void Application::notify_message(std::string message) const {
        m_messages.messages.emplace_back(std::move(message), std::chrono::system_clock::now(), m_messages.sequence);
        m_messages.sequence++;

        while (m_messages.messages.size() > MAX_MESSAGE_COUNT) {
            m_messages.messages.erase(m_messages.messages.begin());
        }
    }

    void Application::update_messages() {
        const auto time_now = std::chrono::system_clock::now();

        for (Message& message : m_messages.messages) {
            if (time_now - message.time > MAX_MESSAGE_DURATION) {
                erase_message(message);
            }

            static constexpr float MAX_DURATION = std::chrono::duration<float>(MAX_MESSAGE_DURATION).count();
            const float elapsed = std::chrono::duration<float>(time_now - message.time).count();

            message.opacity = std::clamp(math::map(elapsed, MAX_DURATION - 1.0f, MAX_DURATION, 1.0f, 0.0f), 0.0f, 1.0f);
        }
    }

    void Application::erase_message(const Message& message) {
        m_task_manager.add_immediate_task([this, sequence = message.sequence] {
            std::erase_if(m_messages.messages, [sequence](const auto& msg) {
                return msg.sequence == sequence;
            });
        });
    }
}
