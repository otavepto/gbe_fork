#ifdef EMU_OVERLAY

// if you're wondering about text like: ##PopupAcceptInvite
// these are unique labels (keys) for each button/label/text,etc...
// ImGui uses the labels as keys, adding a suffic like "My Text##SomeKey"
// avoids confusing ImGui when another label has the same text "MyText"

#include "overlay/steam_overlay.h"
#include "overlay/notification.h"
#include "overlay/steam_overlay_translations.h"

#include <thread>
#include <string>
#include <sstream>
#include <cctype>
#include <utility>

#include "InGameOverlay/ImGui/imgui.h"

#include "dll/dll.h"
#include "dll/settings_parser.h"

#include "InGameOverlay/RendererDetector.h"

// fonts
#include "fonts/NotoSansJP_SemiBold.hpp" // japanese
#include "fonts/NotoSansKR_SemiBold.hpp" // korean
#include "fonts/NotoSansSC_SemiBold.hpp" // simplified chinese
#include "fonts/NotoSansTC_SemiBold.hpp" // traditional chinese
#include "fonts/NotoSansThai_SemiBold.hpp" // Thai
#include "fonts/Roboto_Medium.hpp"

#define URL_WINDOW_NAME "URL Window"

static constexpr int max_window_id = 10000;
static constexpr int base_notif_window_id  = 0 * max_window_id;
static constexpr int base_friend_window_id = 1 * max_window_id;
static constexpr int base_friend_item_id   = 2 * max_window_id;

static const char* valid_languages[] = {
    "english",
    "arabic",
    "bulgarian",
    "schinese",
    "tchinese",
    "czech",
    "danish",
    "dutch",
    "finnish",
    "french",
    "german",
    "greek",
    "hungarian",
    "italian",
    "japanese",
    "koreana",
    "norwegian",
    "polish",
    "portuguese",
    "brazilian",
    "romanian",
    "russian",
    "spanish",
    "latam",
    "swedish",
    "thai",
    "turkish",
    "ukrainian",
    "vietnamese",
    "croatian"
};

ImFontAtlas fonts_atlas{};
ImFont *font_default{};
ImFont *font_notif{};

std::recursive_mutex overlay_mutex{};
std::atomic<bool> setup_overlay_called = false;

char *notif_achievement_wav_custom{};
char *notif_invite_wav_custom{};
bool notif_achievement_wav_custom_inuse = false;
bool notif_invite_wav_custom_inuse = false;


// ListBoxHeader() is deprecated and inlined inside <imgui.h>
// Helper to calculate size from items_count and height_in_items
static inline bool ImGuiHelper_BeginListBox(const char* label, int items_count) {
    int min_items = items_count < 7 ? items_count : 7;
    float height = ImGui::GetTextLineHeightWithSpacing() * (min_items + 0.25f) + ImGui::GetStyle().FramePadding.y * 2.0f;
    return ImGui::BeginListBox(label, ImVec2(0.0f, height));
}


void Steam_Overlay::overlay_run_callback(void* object)
{
    // PRINT_DEBUG("overlay_run_every_runcb %p\n", object);
    Steam_Overlay* _this = reinterpret_cast<Steam_Overlay*>(object);
    _this->steam_run_callback();
}

void Steam_Overlay::overlay_networking_callback(void* object, Common_Message* msg)
{
    Steam_Overlay* _this = reinterpret_cast<Steam_Overlay*>(object);
    _this->networking_msg_received(msg);
}

Steam_Overlay::Steam_Overlay(Settings* settings, SteamCallResults* callback_results, SteamCallBacks* callbacks, RunEveryRunCB* run_every_runcb, Networking* network) :
    settings(settings),
    callback_results(callback_results),
    callbacks(callbacks),
    run_every_runcb(run_every_runcb),
    network(network)
{
    // don't even bother initializing the overlay
    if (settings->disable_overlay) return;

    strncpy(username_text, settings->get_local_name(), sizeof(username_text));

    // we need these copies to show the warning only once, then disable the flag
    // avoid manipulating settings->xxx
    this->warn_forced_setting =
        !settings->disable_overlay_warning_any && !settings->disable_overlay_warning_forced_setting && settings->overlay_warn_forced_setting;
    this->warn_local_save =
        !settings->disable_overlay_warning_any && !settings->disable_overlay_warning_local_save && settings->overlay_warn_local_save;
    this->warn_bad_appid =
        !settings->disable_overlay_warning_any && !settings->disable_overlay_warning_bad_appid && settings->get_local_game_id().AppID() == 0;

    this->disable_user_input = this->warn_forced_setting;

    current_language = 0;
    const char *language = settings->get_language();

    int i = 0;
    for (auto &lang : valid_languages) {
        if (strcmp(lang, language) == 0) {
            current_language = i;
            break;
        }

        ++i;
    }

    run_every_runcb->add(&Steam_Overlay::overlay_run_callback, this);
    this->network->setCallback(CALLBACK_ID_STEAM_MESSAGES, settings->get_local_steam_id(), &Steam_Overlay::overlay_networking_callback, this);
    
}

Steam_Overlay::~Steam_Overlay()
{
    if (settings->disable_overlay) return;

    run_every_runcb->remove(&Steam_Overlay::overlay_run_callback, this);
}

void Steam_Overlay::request_renderer_detector()
{
    bool not_yet = false;
    if (requested_renderer_detector.compare_exchange_weak(not_yet, true)) {
        PRINT_DEBUG("Steam_Overlay::request_renderer_detector\n");
        future_renderer = InGameOverlay::DetectRenderer();
    }
}

void Steam_Overlay::free_renderer_detector()
{
    bool already_requested = true;
    if (requested_renderer_detector.compare_exchange_weak(already_requested, false)) {
        PRINT_DEBUG("Steam_Overlay::free_renderer_detector\n");
        // free detector resources and check for failure
        InGameOverlay::StopRendererDetection();
        InGameOverlay::FreeDetector();
    }
}

void Steam_Overlay::renderer_detector_delay_thread()
{
    if (renderer_detector_delay_finished || !setup_overlay_called) return;

    if (settings->overlay_hook_delay_sec > 0) {
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now() - setup_time).count();
        if (duration < settings->overlay_hook_delay_sec) {
            return;
        } else {
            PRINT_DEBUG("Steam_Overlay::renderer_detector_delay_thread finished waiting for %lli seconds\n", duration);
        }
    }
    
    request_renderer_detector();
    renderer_hook_init_time = std::chrono::high_resolution_clock::now();
    renderer_detector_delay_finished = true;
}

void Steam_Overlay::renderer_hook_init_thread()
{
    if (renderer_hook_init_finished || !renderer_detector_delay_finished) return;
    // stop request will cleanup resources
    if (!setup_overlay_called) return;

    auto duration = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now() - renderer_hook_init_time).count();
    bool hook_ready = future_renderer.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;

    if (!hook_ready) {
        if (duration > settings->overlay_renderer_detector_timeout_sec) { // timeout
            free_renderer_detector();
            renderer_hook_init_finished = true;
            PRINT_DEBUG(
                "Steam_Overlay::renderer_hook_init_thread failed to detect renderer, timeout=%lli seconds, overlay was set up=%i, hook intance state=%i\n",
                duration, (int)setup_overlay_called, (int)hook_ready
            );
        }

        return;
    }

    PRINT_DEBUG("Steam_Overlay::renderer_hook_init_thread hook is ready to be started\n");

    // we get here once we have a renderer detector
    free_renderer_detector();
    renderer_hook_init_finished = true;

    if (!future_renderer.valid()) {
        PRINT_DEBUG("Steam_Overlay::renderer_hook_init_thread failed, future_renderer was invalid!\n");
        return;
    }

    // do a one time initialization
    // std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    _renderer = future_renderer.get();
    PRINT_DEBUG("Steam_Overlay::renderer_hook_init_thread got renderer %p\n", _renderer);
    create_fonts();
    load_audio();
    load_achievements_data();
    
    // setup renderer callbacks
    const static std::set<InGameOverlay::ToggleKey> overlay_toggle_keys = {
        InGameOverlay::ToggleKey::SHIFT, InGameOverlay::ToggleKey::TAB
    };
    auto overlay_toggle_callback = [this]() { open_overlay_hook(true); };
    _renderer->OverlayProc = [this]() { overlay_render_proc(); };
    _renderer->OverlayHookReady = [this](InGameOverlay::OverlayHookState state) {
        PRINT_DEBUG("Steam_Overlay hook state changed to <%i>\n", (int)state);
        overlay_state_hook(state == InGameOverlay::OverlayHookState::Ready || state == InGameOverlay::OverlayHookState::Reset);
    };

    bool started = _renderer->StartHook(overlay_toggle_callback, overlay_toggle_keys, &fonts_atlas);
    PRINT_DEBUG("Steam_Overlay::renderer_hook_init_thread started renderer hook (result=%i)\n", (int)started);
}

void Steam_Overlay::create_fonts()
{
    static std::atomic<bool> create_fonts_called = false;
    bool not_created_yet = false;
    if (!create_fonts_called.compare_exchange_weak(not_created_yet, true)) return;

    // disable rounding the texture height to the next power of two
    // see this: https://github.com/ocornut/imgui/blob/master/docs/FONTS.md#4-font-atlas-texture-fails-to-upload-to-gpu
    fonts_atlas.Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

    float font_size = settings->overlay_appearance.font_size;
    
    static ImFontConfig font_cfg{};
    font_cfg.FontDataOwnedByAtlas = false; // https://github.com/ocornut/imgui/blob/master/docs/FONTS.md#loading-font-data-from-memory
    font_cfg.PixelSnapH = true;
    font_cfg.OversampleH = 1;
    font_cfg.OversampleV = 1;
    font_cfg.SizePixels = font_size;
    // non-latin characters look ugly and squeezed without this horizontal spacing
    font_cfg.GlyphExtraSpacing.x = settings->overlay_appearance.font_glyph_extra_spacing_x; 
    font_cfg.GlyphExtraSpacing.y = settings->overlay_appearance.font_glyph_extra_spacing_y;

    static ImFontGlyphRangesBuilder font_builder{};
    for (auto &x : achievements) {
        font_builder.AddText(x.title.c_str());
        font_builder.AddText(x.description.c_str());
    }
    for (int i = 0; i < TRANSLATION_NUMBER_OF_LANGUAGES; i++) {
        font_builder.AddText(translationChat[i]);
        font_builder.AddText(translationCopyId[i]);
        font_builder.AddText(translationInvite[i]);
        font_builder.AddText(translationInviteAll[i]);
        font_builder.AddText(translationJoin[i]);
        font_builder.AddText(translationInvitedYouToJoinTheGame[i]);
        font_builder.AddText(translationAccept[i]);
        font_builder.AddText(translationRefuse[i]);
        font_builder.AddText(translationSend[i]);
        font_builder.AddText(translationSteamOverlay[i]);
        font_builder.AddText(translationUserPlaying[i]);
        font_builder.AddText(translationRenderer[i]);
        font_builder.AddText(translationShowAchievements[i]);
        font_builder.AddText(translationSettings[i]);
        font_builder.AddText(translationFriends[i]);
        font_builder.AddText(translationAchievementWindow[i]);
        font_builder.AddText(translationListOfAchievements[i]);
        font_builder.AddText(translationAchievements[i]);
        font_builder.AddText(translationHiddenAchievement[i]);
        font_builder.AddText(translationAchievedOn[i]);
        font_builder.AddText(translationNotAchieved[i]);
        font_builder.AddText(translationGlobalSettingsWindow[i]);
        font_builder.AddText(translationGlobalSettingsWindowDescription[i]);
        font_builder.AddText(translationUsername[i]);
        font_builder.AddText(translationLanguage[i]);
        font_builder.AddText(translationSelectedLanguage[i]);
        font_builder.AddText(translationRestartTheGameToApply[i]);
        font_builder.AddText(translationSave[i]);
        font_builder.AddText(translationWarning[i]);
        font_builder.AddText(translationWarningWarningWarning[i]);
        font_builder.AddText(translationWarningDescription1[i]);
        font_builder.AddText(translationWarningDescription2[i]);
        font_builder.AddText(translationWarningDescription3[i]);
        font_builder.AddText(translationWarningDescription4[i]);
        font_builder.AddText(translationSteamOverlayURL[i]);
        font_builder.AddText(translationClose[i]);
        font_builder.AddText(translationPlaying[i]);
        font_builder.AddText(translationAutoAcceptFriendInvite[i]);
    }
    font_builder.AddRanges(fonts_atlas.GetGlyphRangesDefault());

    static ImVector<ImWchar> ranges{};
    font_builder.BuildRanges(&ranges);
    font_cfg.GlyphRanges = ranges.Data;

    fonts_atlas.AddFontDefault(&font_cfg);
    font_cfg.MergeMode = true; // merge next fonts into the first one, as if they were all just 1 font file
    fonts_atlas.AddFontFromMemoryCompressedTTF(NotoSansJP_SemiBold_compressed_data, NotoSansJP_SemiBold_compressed_size, font_size, &font_cfg);
    fonts_atlas.AddFontFromMemoryCompressedTTF(NotoSansKR_SemiBold_compressed_data, NotoSansKR_SemiBold_compressed_size, font_size, &font_cfg);
    fonts_atlas.AddFontFromMemoryCompressedTTF(NotoSansSC_SemiBold_compressed_data, NotoSansSC_SemiBold_compressed_size, font_size, &font_cfg);
    fonts_atlas.AddFontFromMemoryCompressedTTF(NotoSansTC_SemiBold_compressed_data, NotoSansTC_SemiBold_compressed_size, font_size, &font_cfg);
    fonts_atlas.AddFontFromMemoryCompressedTTF(NotoSansThai_SemiBold_compressed_data, NotoSansThai_SemiBold_compressed_size, font_size, &font_cfg);
    ImFont *font = fonts_atlas.AddFontFromMemoryCompressedTTF(Roboto_Medium_compressed_data, Roboto_Medium_compressed_size, font_size, &font_cfg);
    font_notif = font_default = font;
    
    bool res = fonts_atlas.Build();
    PRINT_DEBUG("Steam_Overlay::create_fonts created fonts atlas (result=%i)\n", (int)res);

    reset_LastError();
}

void Steam_Overlay::load_audio()
{
    std::string file_path{};
    std::string file_name{};
    unsigned long long file_size;

    for (int i = 0; i < 2; i++) {
        if (i == 0) file_name = "overlay_achievement_notification.wav";
        if (i == 1) file_name = "overlay_friend_notification.wav";

        file_path = Local_Storage::get_game_settings_path() + file_name;
        file_size = file_size_(file_path);
        if (!file_size) {
            if (settings->local_save.length() > 0) {
                file_path = settings->local_save + "/settings/" + file_name;
            } else {
                file_path = Local_Storage::get_user_appdata_path() + "/settings/" + file_name;
            }
            file_size = file_size_(file_path);
        }
        if (file_size) {
            std::ifstream myfile;
            myfile.open(utf8_decode(file_path), std::ios::binary | std::ios::in);
            if (myfile.is_open()) {
                myfile.seekg (0, myfile.end);
                int length = myfile.tellg();
                myfile.seekg (0, myfile.beg);

                if (i == 0) {
                    notif_achievement_wav_custom = new char [length];
                    myfile.read (notif_achievement_wav_custom, length);
                    notif_achievement_wav_custom_inuse = true;
                }
                if (i == 1) {
                    notif_invite_wav_custom = new char [length];
                    myfile.read (notif_invite_wav_custom, length);
                    notif_invite_wav_custom_inuse = true;
                }

                myfile.close();
            }
        }
    }
}

void Steam_Overlay::load_achievements_data()
{
    PRINT_DEBUG("Steam_Overlay::load_achievements_data\n");
    std::lock_guard<std::recursive_mutex> lock(global_mutex);

    Steam_User_Stats* steamUserStats = get_steam_client()->steam_user_stats;
    uint32 achievements_num = steamUserStats->GetNumAchievements();
    for (uint32 i = 0; i < achievements_num; ++i) {
        Overlay_Achievement ach{};
        ach.name = steamUserStats->GetAchievementName(i);
        ach.title = steamUserStats->GetAchievementDisplayAttribute(ach.name.c_str(), "name");
        ach.description = steamUserStats->GetAchievementDisplayAttribute(ach.name.c_str(), "desc");
        
        const char *hidden = steamUserStats->GetAchievementDisplayAttribute(ach.name.c_str(), "hidden");
        ach.hidden = hidden && hidden[0] == '1';

        bool achieved = false;
        uint32 unlock_time = 0;
        if (steamUserStats->GetAchievementAndUnlockTime(ach.name.c_str(), &achieved, &unlock_time)) {
            ach.achieved = achieved;
            ach.unlock_time = unlock_time;
        } else {
            ach.achieved = false;
            ach.unlock_time = 0;
        }

        ach.icon_name = steamUserStats->get_achievement_icon_name(ach.name.c_str(), true);
        ach.icon_gray_name = steamUserStats->get_achievement_icon_name(ach.name.c_str(), false);

        achievements.push_back(ach);
        
        if (!setup_overlay_called) return;
    }

    PRINT_DEBUG("Steam_Overlay::load_achievements_data count=%u, loaded=%zu\n", achievements_num, achievements.size());

}

void Steam_Overlay::initial_load_achievements_icons()
{
    {
        std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
        if (late_init_ach_icons) return;
    }

    PRINT_DEBUG("Steam_Overlay::initial_load_achievements_icons\n");
    for (auto &ach : achievements) {
        {
            std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
            if (!is_ready || !setup_overlay_called) {
                PRINT_DEBUG("Steam_Overlay::initial_load_achievements_icons early exit\n");
                return;
            }
        }

        try_load_ach_icon(ach);

        {
            std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
            if (!is_ready || !setup_overlay_called) {
                PRINT_DEBUG("Steam_Overlay::initial_load_achievements_icons early exit\n");
                return;
            }
        }

        try_load_ach_gray_icon(ach);
    }

    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    late_init_ach_icons = true;
}

// called initially and when window size is updated
void Steam_Overlay::overlay_state_hook(bool ready)
{
    PRINT_DEBUG("Steam_Overlay::overlay_state_hook %i\n", (int)ready);
    
    // NOTE usage of local objects here cause an exception when this is called with false state
    // the reason is that by the time this hook is called, the object may have been already destructed
    // this is why we use global mutex
    // TODO this also doesn't seem right, no idea why it happens though
    // NOTE after initializing the renderer detector on another thread this was solved
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!setup_overlay_called) return;

    is_ready = ready;

    if (ready) {
        // Antichamber may crash here because ImGui Context is null!, no idea why
        bool not_yet = false;
        if (ImGui::GetCurrentContext() && late_init_imgui.compare_exchange_weak(not_yet, true)) {
            PRINT_DEBUG("Steam_Overlay::overlay_state_hook late init ImGui\n");

            ImGuiIO &io = ImGui::GetIO();
            // disable loading the default ini file
            io.IniFilename = NULL;

            ImGuiStyle &style = ImGui::GetStyle();
            // Disable round window
            style.WindowRounding = 0.0;
        }
    }
}

// called when the user presses SHIFT + TAB
bool Steam_Overlay::open_overlay_hook(bool toggle)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (toggle) {
        ShowOverlay(!show_overlay);
    }

    return show_overlay;
}

void Steam_Overlay::allow_renderer_frame_processing(bool state, bool cleaning_up_overlay)
{
    // this is very important internally it calls the necessary fuctions
    // to properly update ImGui window size on the next overlay_render_proc() call

    if (state) {
        auto new_val = ++renderer_frame_processing_requests;
        if (new_val == 1) { // only take an action on first request
            // allow internal frmae processing
            _renderer->HideOverlayInputs(false);
            PRINT_DEBUG("Steam_Overlay::allow_renderer_frame_processing enabled frame processing (count=%u)\n", new_val);
        }
    } else {
        if (renderer_frame_processing_requests > 0) {
            auto new_val = --renderer_frame_processing_requests;
            if (!new_val || cleaning_up_overlay) { // only take an action when the requests reach 0 or by force
                _renderer->HideOverlayInputs(true);
                PRINT_DEBUG("Steam_Overlay::allow_renderer_frame_processing disabled frame processing (count=%u, force=%i)\n", new_val, (int)cleaning_up_overlay);
            }
        }
    }
}

void Steam_Overlay::obscure_game_input(bool state) {
    if (state) {
        auto new_val = ++obscure_cursor_requests;
        if (new_val == 1) { // only take an action on first request
            ImGuiIO &io = ImGui::GetIO();
            // force draw the cursor, otherwise games like Truberbrook will not have an overlay cursor
            io.MouseDrawCursor = state;
            // not necessary, just to be sure
            io.WantCaptureMouse = state;
            // not necessary, just to be sure
            io.WantCaptureKeyboard = state;

            // clip the cursor
            _renderer->HideAppInputs(true);
            PRINT_DEBUG("Steam_Overlay::obscure_game_input obscured app input (count=%u)\n", new_val);
        }
    } else {
        if (obscure_cursor_requests > 0) {
            auto new_val = --obscure_cursor_requests;
            if (!new_val) { // only take an action when the requests reach 0
                ImGuiIO &io = ImGui::GetIO();
                // force draw the cursor, otherwise games like Truberbrook will not have an overlay cursor
                io.MouseDrawCursor = state;
                // not necessary, just to be sure
                io.WantCaptureMouse = state;
                // not necessary, just to be sure
                io.WantCaptureKeyboard = state;
                
                // restore the old cursor
                _renderer->HideAppInputs(false);
                PRINT_DEBUG("Steam_Overlay::obscure_game_input restored app input (count=%u)\n", new_val);
            }
        }
    }
}

void Steam_Overlay::notify_sound_user_invite(friend_window_state& friend_state)
{
    if (settings->disable_overlay_friend_notification) return;
    if (!(friend_state.window_state & window_state_show) || !show_overlay)
    {
        friend_state.window_state |= window_state_need_attention;
#ifdef __WINDOWS__
        if (notif_invite_wav_custom_inuse) {
            PlaySoundA((LPCSTR)notif_invite_wav_custom, NULL, SND_ASYNC | SND_MEMORY);
        } else {
            PlaySoundA((LPCSTR)notif_invite_wav, NULL, SND_ASYNC | SND_MEMORY);
        }
#endif
    }
}

void Steam_Overlay::notify_sound_user_achievement()
{
    if (settings->disable_overlay_achievement_notification) return;
    if (!show_overlay)
    {
#ifdef __WINDOWS__
        if (notif_achievement_wav_custom_inuse) {
            PlaySoundA((LPCSTR)notif_achievement_wav_custom, NULL, SND_ASYNC | SND_MEMORY);
        }
#endif
    }
}

void Steam_Overlay::notify_sound_auto_accept_friend_invite()
{
#ifdef __WINDOWS__
    if (notif_achievement_wav_custom_inuse) {
        PlaySoundA((LPCSTR)notif_achievement_wav_custom, NULL, SND_ASYNC | SND_MEMORY);
    } else {
        PlaySoundA((LPCSTR)notif_invite_wav, NULL, SND_ASYNC | SND_MEMORY);
    }
#endif
}

int find_free_id(std::vector<int> & ids, int base)
{
    std::sort(ids.begin(), ids.end());

    int id = base;
    for (auto i : ids)
    {
        if (id < i)
            break;
        id = i + 1;
    }

    return id > (base+max_window_id) ? 0 : id;
}

int find_free_friend_id(const std::map<Friend, friend_window_state, Friend_Less> &friend_windows)
{
    std::vector<int> ids{};
    ids.reserve(friend_windows.size());

    std::for_each(friend_windows.begin(), friend_windows.end(), [&ids](std::pair<Friend const, friend_window_state> const& i)
    {
        ids.emplace_back(i.second.id);
    });
    
    return find_free_id(ids, base_friend_window_id);
}

int find_free_notification_id(std::vector<Notification> const& notifications)
{
    std::vector<int> ids{};
    ids.reserve(notifications.size());

    std::for_each(notifications.begin(), notifications.end(), [&ids](Notification const& i)
    {
        ids.emplace_back(i.id);
    });
    

    return find_free_id(ids, base_friend_window_id);
}

bool Steam_Overlay::submit_notification(notification_type type, const std::string &msg, std::pair<const Friend, friend_window_state> *frd, const std::weak_ptr<uint64_t> &icon)
{
    PRINT_DEBUG("Steam_Overlay::submit_notification %i\n", (int)type);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return false;
    
    int id = find_free_notification_id(notifications);
    if (id == 0) {
        PRINT_DEBUG("Steam_Overlay::submit_notification error no free id to create a notification window\n");
        return false;
    }

    Notification notif{};
    notif.start_time = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch());
    notif.id = id;
    notif.type = type;
    notif.message = msg;
    notif.frd = frd;
    notif.icon = icon;
    
    notifications.emplace_back(notif);
    allow_renderer_frame_processing(true);
    // uncomment this block to obscure cursor input and steal focus for these specific notifications
    switch (type) {
        // we want to steal focus for these ones
        case notification_type_invite:
            obscure_game_input(true);
        break;

        // not effective
        case notification_type_achievement:
        case notification_type_auto_accept_invite:
        case notification_type_message:
            // nothing
        break;

        default:
            PRINT_DEBUG("Steam_Overlay::submit_notification error unhandled type %i\n", (int)type);
        break;
    }

    return true;
}

void Steam_Overlay::add_chat_message_notification(std::string const &message)
{
    PRINT_DEBUG("Steam_Overlay::add_chat_message_notification '%s'\n", message.c_str());
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (settings->disable_overlay_friend_notification) return;

    submit_notification(notification_type_message, message);
}

bool Steam_Overlay::is_friend_joinable(std::pair<const Friend, friend_window_state> &f)
{
    PRINT_DEBUG("Steam_Overlay::is_friend_joinable " "%" PRIu64 "\n", f.first.id());
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    Steam_Friends* steamFriends = get_steam_client()->steam_friends;

    if (std::string(steamFriends->GetFriendRichPresence((uint64)f.first.id(), "connect")).length() > 0 ) {
        PRINT_DEBUG("Steam_Overlay::is_friend_joinable " "%" PRIu64 " true (connect string)\n", f.first.id());
        return true;
    }

    FriendGameInfo_t friend_game_info{};
    steamFriends->GetFriendGamePlayed((uint64)f.first.id(), &friend_game_info);
    if (friend_game_info.m_steamIDLobby.IsValid() && (f.second.window_state & window_state_lobby_invite)) {
        PRINT_DEBUG("Steam_Overlay::is_friend_joinable " "%" PRIu64 " true (friend in a game)\n", f.first.id());
        return true;
    }

    PRINT_DEBUG("Steam_Overlay::is_friend_joinable " "%" PRIu64 " false\n", f.first.id());
    return false;
}

bool Steam_Overlay::got_lobby()
{
    std::lock_guard<std::recursive_mutex> lock(global_mutex);
    Steam_Friends* steamFriends = get_steam_client()->steam_friends;
    if (std::string(steamFriends->GetFriendRichPresence(settings->get_local_steam_id(), "connect")).length() > 0)
        return true;

    if (settings->get_lobby().IsValid())
        return true;

    return false;
}

void Steam_Overlay::build_friend_context_menu(Friend const& frd, friend_window_state& state)
{
    if (ImGui::BeginPopupContextItem("Friends_ContextMenu", 1)) {
        // this is set to true if any button was clicked
        // otherwise, after clicking any button, the menu will be persistent
        bool close_popup = false;

        // user clicked on "chat"
        if (ImGui::Button(translationChat[current_language])) {
            close_popup = true;
            state.window_state |= window_state_show;
        }
        // user clicked on "copy id" on a friend
        if (ImGui::Button(translationCopyId[current_language])) {
            close_popup = true;
            auto friend_id_str = std::to_string(frd.id());
            ImGui::SetClipboardText(friend_id_str.c_str());
        }
        // If we have the same appid, activate the invite/join buttons
        if (settings->get_local_game_id().AppID() == frd.appid()) {
            // user clicked on "invite to game"
            std::string translationInvite_tmp(translationInvite[current_language]);
            translationInvite_tmp.append("##PopupInviteToGame");
            if (i_have_lobby && ImGui::Button(translationInvite_tmp.c_str())) {
                close_popup = true;
                state.window_state |= window_state_invite;
                has_friend_action.push(frd);
            }
            
            // user clicked on "accept game invite"
            std::string translationJoin_tmp(translationJoin[current_language]);
            translationJoin_tmp.append("##PopupAcceptInvite");
            if (state.joinable && ImGui::Button(translationJoin_tmp.c_str())) {
                close_popup = true;
                // don't bother adding this friend if the button "invite all" was clicked
                // we will send them the invitation later in Steam_Overlay::steam_run_callback()
                if (!invite_all_friends_clicked) {
                    state.window_state |= window_state_join;
                    has_friend_action.push(frd);
                }
            }
        }

        if (close_popup || invite_all_friends_clicked) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void Steam_Overlay::build_friend_window(Friend const& frd, friend_window_state& state)
{
    if (!(state.window_state & window_state_show))
        return;

    bool show = true;
    bool send_chat_msg = false;

    float width = ImGui::CalcTextSize("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA").x;
    
    if (state.window_state & window_state_need_attention && ImGui::IsWindowFocused()) {
        state.window_state &= ~window_state_need_attention;
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2{ width, ImGui::GetFontSize()*8 + ImGui::GetFrameHeightWithSpacing()*4 },
        ImVec2{ std::numeric_limits<float>::max() , std::numeric_limits<float>::max() });

    ImGui::SetNextWindowBgAlpha(0.9f);
    // Window id is after the ###, the window title is the friend name
    std::string friend_window_id = std::move("###" + std::to_string(state.id));
    if (ImGui::Begin((state.window_title + friend_window_id).c_str(), &show)) {
        if (state.window_state & window_state_need_attention && ImGui::IsWindowFocused())
        {
            state.window_state &= ~window_state_need_attention;
        }

        // Fill this with the chat box and maybe the invitation
        if (state.window_state & (window_state_lobby_invite | window_state_rich_invite))
        {
            ImGui::LabelText("##label", translationInvitedYouToJoinTheGame[current_language], frd.name().c_str(), frd.appid());
            ImGui::SameLine();
            if (ImGui::Button(translationAccept[current_language]))
            {
                state.window_state |= window_state_join;
                this->has_friend_action.push(frd);
            }
            ImGui::SameLine();
            if (ImGui::Button(translationRefuse[current_language]))
            {
                state.window_state &= ~(window_state_lobby_invite | window_state_rich_invite);
            }
        }

        ImGui::InputTextMultiline("##chat_history", &state.chat_history[0], state.chat_history.length(), { -1.0f, -2.0f * ImGui::GetFontSize() }, ImGuiInputTextFlags_ReadOnly);
        // TODO: Fix the layout of the chat line + send button.
        // It should be like this: chat input should fill the window size minus send button size (button size is fixed)
        // |------------------------------|
        // | /--------------------------\ |
        // | |                          | |
        // | |       chat history       | |
        // | |                          | |
        // | \--------------------------/ |
        // | [____chat line______] [send] |
        // |------------------------------|
        //
        // And it is like this
        // |------------------------------|
        // | /--------------------------\ |
        // | |                          | |
        // | |       chat history       | |
        // | |                          | |
        // | \--------------------------/ |
        // | [__chat line__] [send]       |
        // |------------------------------|
        float wnd_width = ImGui::GetContentRegionAvail().x;
        ImGuiStyle &style = ImGui::GetStyle();
        wnd_width -= ImGui::CalcTextSize(translationSend[current_language]).x + style.FramePadding.x * 2 + style.ItemSpacing.x + 1;

        uint64_t frd_id = frd.id();
        ImGui::PushID((const char *)&frd_id, (const char *)&frd_id + sizeof(frd_id));
        ImGui::PushItemWidth(wnd_width);

        if (ImGui::InputText("##chat_line", state.chat_input, max_chat_len, ImGuiInputTextFlags_EnterReturnsTrue))
        {
            send_chat_msg = true;
            ImGui::SetKeyboardFocusHere(-1);
        }
        ImGui::PopItemWidth();
        ImGui::PopID();

        ImGui::SameLine();

        if (ImGui::Button(translationSend[current_language]))
        {
            send_chat_msg = true;
        }

        if (send_chat_msg)
        {
            if (!(state.window_state & window_state_send_message))
            {
                has_friend_action.push(frd);
                state.window_state |= window_state_send_message;
            }
        }
    }
    
    // User closed the friend window
    if (!show)
        state.window_state &= ~window_state_show;

    ImGui::End();
}

// set the position of the next notification
void Steam_Overlay::set_next_notification_pos(float width, float height, float font_size, notification_type type, struct NotificationsIndexes &idx)
{
    // 0 on the y-axis is top, 0 on the x-axis is left

    // get the required position
    Overlay_Appearance::NotificationPosition pos = Overlay_Appearance::default_pos;
    switch (type) {
    case notification_type::notification_type_achievement: pos = settings->overlay_appearance.ach_earned_pos; break;
    case notification_type::notification_type_invite: pos = settings->overlay_appearance.invite_pos; break;
    case notification_type::notification_type_message: pos = settings->overlay_appearance.chat_msg_pos; break;
    default: /* satisfy compiler warning */ break;
    }

    float x = 0.0f;
    float y = 0.0f;
    const float noti_width = width * Notification::width_percent;
    const float noti_height = Notification::height * font_size;
    switch (pos) {
    // top
    case Overlay_Appearance::NotificationPosition::top_left:
        x = 0.0f;
        y = noti_height * idx.top_left;
        ++idx.top_left;
    break;
    case Overlay_Appearance::NotificationPosition::top_center:
        x = (width / 2) - (noti_width / 2);
        y = noti_height * idx.top_center;
        ++idx.top_center;
    break;
    case Overlay_Appearance::NotificationPosition::top_right:
        x = width - noti_width;
        y = noti_height * idx.top_right;
        ++idx.top_right;
    break;
    
    // bot
    case Overlay_Appearance::NotificationPosition::bot_left:
        x = 0.0f;
        y = height - noti_height * (idx.bot_left + 1);
        ++idx.bot_left;
    break;
    case Overlay_Appearance::NotificationPosition::bot_center:
        x = (width / 2) - (noti_width / 2);
        y = height - noti_height * (idx.bot_center + 1);
        ++idx.bot_center;
    break;
    case Overlay_Appearance::NotificationPosition::bot_right:
        x = width - noti_width;
        y = height - noti_height * (idx.bot_right + 1);
        ++idx.bot_right;
    break;
    
    default: /* satisfy compiler warning */ break;
    }

    ImGui::SetNextWindowPos(ImVec2( x, y ));
}

void Steam_Overlay::build_notifications(int width, int height)
{
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    float font_size = ImGui::GetFontSize();
    std::queue<Friend> friend_actions_temp{};

    NotificationsIndexes idx{};
    for (auto it = notifications.begin(); it != notifications.end(); ++it) {
        auto elapsed_notif = now - it->start_time;

        set_next_notification_pos(width, height, font_size, (notification_type)it->type, idx);
        ImGui::SetNextWindowSize(ImVec2( width * Notification::width_percent, Notification::height * font_size ));
        ImGui::SetNextWindowBgAlpha(0.9f);
        
        if ( elapsed_notif < Notification::fade_in) { // still appearing (fading in)
            float alpha = settings->overlay_appearance.notification_a * (elapsed_notif.count() / static_cast<float>(Notification::fade_in.count()));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, alpha));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(settings->overlay_appearance.notification_r, settings->overlay_appearance.notification_g, settings->overlay_appearance.notification_b, alpha));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(255, 255, 255, alpha*2));
        } else if ( elapsed_notif > Notification::fade_out_start) { // fading out 
            float alpha = settings->overlay_appearance.notification_a * ((Notification::show_time - elapsed_notif).count() / static_cast<float>(Notification::fade_out.count()));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, alpha));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(settings->overlay_appearance.notification_r, settings->overlay_appearance.notification_g, settings->overlay_appearance.notification_b, alpha));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(255, 255, 255, alpha*2));
        } else { // still in the visible time limit
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, settings->overlay_appearance.notification_a));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(settings->overlay_appearance.notification_r, settings->overlay_appearance.notification_g, settings->overlay_appearance.notification_b, settings->overlay_appearance.notification_a));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(255, 255, 255, settings->overlay_appearance.notification_a*2));
        }
        
        // some extra window flags for each notification type
        ImGuiWindowFlags extra_flags = ImGuiWindowFlags_NoFocusOnAppearing;
        switch (it->type) {
            // games like "Mafia Definitive Edition" will pause the entire game/scene if focus was stolen
            // be less intrusive for notifications that do not require interaction
            case notification_type_achievement:
            case notification_type_auto_accept_invite:
            case notification_type_message:
                extra_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs;
            break;

            case notification_type_invite:
                // nothing
            break;

            default:
                PRINT_DEBUG("Steam_Overlay::build_notifications error unhandled flags for type %i\n", (int)it->type);
            break;
        }

        std::string wnd_name = "NotiPopupShow" + std::to_string(it->id);
        if (ImGui::Begin(wnd_name.c_str(), nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | extra_flags)) {
            switch (it->type) {
                case notification_type_achievement: {
                    if (!it->icon.expired() && ImGui::BeginTable("imgui_table", 2)) {
                        ImGui::TableSetupColumn("imgui_table_image", ImGuiTableColumnFlags_WidthFixed, settings->overlay_appearance.icon_size);
                        ImGui::TableSetupColumn("imgui_table_text");
                        ImGui::TableNextRow(ImGuiTableRowFlags_None, settings->overlay_appearance.icon_size);

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Image((ImTextureID)*it->icon.lock().get(), ImVec2(settings->overlay_appearance.icon_size, settings->overlay_appearance.icon_size));

                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextWrapped("%s", it->message.c_str());

                        ImGui::EndTable();
                    } else {
                        ImGui::TextWrapped("%s", it->message.c_str());
                    }
                }
                break;

                case notification_type_invite: {
                    ImGui::TextWrapped("%s", it->message.c_str());
                    if (ImGui::Button(translationJoin[current_language]))
                    {
                        it->frd->second.window_state |= window_state_join;
                        friend_actions_temp.push(it->frd->first);
                        it->start_time = std::chrono::seconds(0);
                    }
                }
                break;

                case notification_type_message:
                    ImGui::TextWrapped("%s", it->message.c_str());
                break;

                case notification_type_auto_accept_invite:
                    ImGui::TextWrapped("%s", it->message.c_str());
                break;
                
                default:
                    PRINT_DEBUG("Steam_Overlay::build_notifications error unhandled notification for type %i\n", (int)it->type);
                break;
            }

        }

        ImGui::End();

        ImGui::PopStyleColor(3);
    }

    // erase all notifications whose visible time exceeded the max
    notifications.erase(std::remove_if(notifications.begin(), notifications.end(), [this, &now](Notification &item) {
        if ((now - item.start_time) > Notification::show_time) {
            PRINT_DEBUG("Steam_Overlay::build_notifications removing a notification\n");
            allow_renderer_frame_processing(false);
            // uncomment this block to restore app input focus
            switch (item.type) {
                // we want to restore focus for these ones
                case notification_type_invite:
                    obscure_game_input(false);
                break;

                // not effective
                case notification_type_achievement:
                case notification_type_auto_accept_invite:
                case notification_type_message:
                    // nothing
                break;

                default:
                    PRINT_DEBUG("Steam_Overlay::build_notifications error unhandled remove for type %i\n", (int)item.type);
                break;
            }

            return true;
        }
        
        return false;
    }), notifications.end());

    if (!friend_actions_temp.empty()) {
        while (!friend_actions_temp.empty()) {
            has_friend_action.push(friend_actions_temp.front());
            friend_actions_temp.pop();
        }
    }
}

void Steam_Overlay::add_auto_accept_invite_notification()
{
    PRINT_DEBUG("Steam_Overlay::add_auto_accept_invite_notification\n");
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;
    
    char tmp[TRANSLATION_BUFFER_SIZE]{};
    snprintf(tmp, sizeof(tmp), "%s", translationAutoAcceptFriendInvite[current_language]);
    
    submit_notification(notification_type_auto_accept_invite, tmp);
    notify_sound_auto_accept_friend_invite();
}

void Steam_Overlay::add_invite_notification(std::pair<const Friend, friend_window_state>& wnd_state)
{
    PRINT_DEBUG("Steam_Overlay::add_invite_notification\n");
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (settings->disable_overlay_friend_notification) return;
    if (!Ready()) return;
    
    char tmp[TRANSLATION_BUFFER_SIZE]{};
    auto &first_friend = wnd_state.first;
    auto &name = first_friend.name();
    snprintf(tmp, sizeof(tmp), translationInvitedYouToJoinTheGame[current_language], name.c_str(), (uint64)first_friend.id());
    
    submit_notification(notification_type_invite, tmp, &wnd_state);
}

void Steam_Overlay::invite_friend(uint64 friend_id, class Steam_Friends* steamFriends, class Steam_Matchmaking* steamMatchmaking)
{
    std::string connect_str = steamFriends->GetFriendRichPresence(settings->get_local_steam_id(), "connect");
    if (connect_str.length() > 0) {
        steamFriends->InviteUserToGame(friend_id, connect_str.c_str());
        PRINT_DEBUG("Steam_Overlay sent game invitation to friend with id = %llu\n", friend_id);
    } else if (settings->get_lobby().IsValid()) {
        steamMatchmaking->InviteUserToLobby(settings->get_lobby(), friend_id);
        PRINT_DEBUG("Steam_Overlay sent lobby invitation to friend with id = %llu\n", friend_id);
    }
}

bool Steam_Overlay::try_load_ach_icon(Overlay_Achievement &ach)
{
    if (!_renderer) return false;
    if (!ach.icon.expired()) return true;
    
    if (ach.icon_load_trials && ach.icon_name.size()) {
        --ach.icon_load_trials;
        std::string file_path = std::move(Local_Storage::get_game_settings_path() + ach.icon_name);
        unsigned long long file_size = file_size_(file_path);
        if (!file_size) {
            file_path = std::move(Local_Storage::get_game_settings_path() + Steam_Overlay::ACH_FALLBACK_DIR + "/" + ach.icon_name);
            file_size = file_size_(file_path);
        }
        if (file_size) {
            std::string img = Local_Storage::load_image_resized(file_path, "", settings->overlay_appearance.icon_size);
            if (img.length() > 0) {
                ach.icon = _renderer->CreateImageResource(
                    (void*)img.c_str(),
                    settings->overlay_appearance.icon_size, settings->overlay_appearance.icon_size);
                if (!ach.icon.expired()) ach.icon_load_trials = Overlay_Achievement::ICON_LOAD_MAX_TRIALS;
                PRINT_DEBUG("Steam_Overlay::try_load_ach_icon '%s' (result=%i)\n", ach.name.c_str(), (int)!ach.icon.expired());
            }
        }
    }

    return !ach.icon.expired();
}

bool Steam_Overlay::try_load_ach_gray_icon(Overlay_Achievement &ach)
{
    if (!_renderer) return false;
    if (!ach.icon_gray.expired()) return true;
    
    if (ach.icon_gray_load_trials && ach.icon_gray_name.size()) {
        --ach.icon_gray_load_trials;
        std::string file_path = std::move(Local_Storage::get_game_settings_path() + ach.icon_gray_name);
        unsigned long long file_size = file_size_(file_path);
        if (!file_size) {
            file_path = std::move(Local_Storage::get_game_settings_path() + Steam_Overlay::ACH_FALLBACK_DIR + "/" + ach.icon_gray_name);
            file_size = file_size_(file_path);
        }
        if (file_size) {
            std::string img = Local_Storage::load_image_resized(file_path, "", settings->overlay_appearance.icon_size);
            if (img.length() > 0) {
                ach.icon_gray = _renderer->CreateImageResource(
                    (void*)img.c_str(),
                    settings->overlay_appearance.icon_size, settings->overlay_appearance.icon_size);
                if (!ach.icon_gray.expired()) ach.icon_gray_load_trials = Overlay_Achievement::ICON_LOAD_MAX_TRIALS;
                PRINT_DEBUG("Steam_Overlay::try_load_ach_gray_icon '%s' (result=%i)\n", ach.name.c_str(), (int)!ach.icon_gray.expired());
            }
        }
    }

    return !ach.icon_gray.expired();
}

// Try to make this function as short as possible or it might affect game's fps.
void Steam_Overlay::overlay_render_proc()
{
    initial_load_achievements_icons();
    
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    ImGuiIO &io = ImGui::GetIO();

    // Set the overlay windows to the size of the game window
    // only if we have a reason (full overlay or just some notification)
    if (show_overlay || notifications.size()) {
        ImGui::SetNextWindowPos({ 0,0 });
        ImGui::SetNextWindowSize({ io.DisplaySize.x, io.DisplaySize.y });
        ImGui::SetNextWindowBgAlpha(0.55f);
    }

    if (show_overlay) {
        render_main_window();
    }

    if (notifications.size()) {
        ImGui::PushFont(font_notif);
        build_notifications(io.DisplaySize.x, io.DisplaySize.y);
        ImGui::PopFont();
    }

}

// Try to make this function as short as possible or it might affect game's fps.
void Steam_Overlay::render_main_window()
{
    //ImGui::SetNextWindowFocus();
    ImGui::PushFont(font_default);
    bool show = true;

    char tmp[TRANSLATION_BUFFER_SIZE]{};
    snprintf(tmp, sizeof(tmp), translationRenderer[current_language], (_renderer == nullptr ? "Unknown" : _renderer->GetLibraryName().c_str()));
    std::string windowTitle;
    windowTitle.append(translationSteamOverlay[current_language]);
    windowTitle.append(" (");
    windowTitle.append(tmp);
    windowTitle.append(")");

    if ((settings->overlay_appearance.background_r >= 0) &&
        (settings->overlay_appearance.background_g >= 0) &&
        (settings->overlay_appearance.background_b >= 0) &&
        (settings->overlay_appearance.background_a >= 0)) {
        ImVec4 colorSet = ImVec4(
            settings->overlay_appearance.background_r,
            settings->overlay_appearance.background_g,
            settings->overlay_appearance.background_b,
            settings->overlay_appearance.background_a
        );
        ImGui::PushStyleColor(ImGuiCol_WindowBg, colorSet);
    }

    if ((settings->overlay_appearance.element_r >= 0) &&
        (settings->overlay_appearance.element_g >= 0) &&
        (settings->overlay_appearance.element_b >= 0) &&
        (settings->overlay_appearance.element_a >= 0)) {
        ImVec4 colorSet = ImVec4(
            settings->overlay_appearance.element_r,
            settings->overlay_appearance.element_g,
            settings->overlay_appearance.element_b,
            settings->overlay_appearance.element_a
        );
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, colorSet);
        ImGui::PushStyleColor(ImGuiCol_Button, colorSet);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, colorSet);
        ImGui::PushStyleColor(ImGuiCol_ResizeGrip, colorSet);
    }

    if ((settings->overlay_appearance.element_hovered_r >= 0) &&
        (settings->overlay_appearance.element_hovered_g >= 0) &&
        (settings->overlay_appearance.element_hovered_b >= 0) &&
        (settings->overlay_appearance.element_hovered_a >= 0)) {
        ImVec4 colorSet = ImVec4(
            settings->overlay_appearance.element_hovered_r,
            settings->overlay_appearance.element_hovered_g,
            settings->overlay_appearance.element_hovered_b,
            settings->overlay_appearance.element_hovered_a
        );
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colorSet);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, colorSet);
        ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, colorSet);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, colorSet);
    }

    if ((settings->overlay_appearance.element_active_r >= 0) &&
        (settings->overlay_appearance.element_active_g >= 0) &&
        (settings->overlay_appearance.element_active_b >= 0) &&
        (settings->overlay_appearance.element_active_a >= 0)) {
        ImVec4 colorSet = ImVec4(
            settings->overlay_appearance.element_active_r,
            settings->overlay_appearance.element_active_g,
            settings->overlay_appearance.element_active_b,
            settings->overlay_appearance.element_active_a
        );
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, colorSet);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, colorSet);
        ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, colorSet);
        ImGui::PushStyleColor(ImGuiCol_Header, colorSet);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, colorSet);
    }

    if (ImGui::Begin(windowTitle.c_str(), &show,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus)) {
        ImGui::LabelText("##playinglabel", translationUserPlaying[current_language],
            settings->get_local_name(),
            settings->get_local_steam_id().ConvertToUint64(),
            settings->get_local_game_id().AppID());

        ImGui::SameLine();
        ImGui::Spacing();
        // user clicked on "show achievements"
        if (ImGui::Button(translationShowAchievements[current_language])) {
            show_achievements = true;
        }

        ImGui::SameLine();
        // user clicked on "settings"
        if (ImGui::Button(translationSettings[current_language])) {
            show_settings = true;
        }
        
        ImGui::SameLine();
        // user clicked on "copy id" on themselves
        if (ImGui::Button(translationCopyId[current_language])) {
            auto friend_id_str = std::to_string(settings->get_local_steam_id().ConvertToUint64());
            ImGui::SetClipboardText(friend_id_str.c_str());
        }

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::LabelText("##label", "%s", translationFriends[current_language]);

        if (!friends.empty()) {
            if (i_have_lobby) {
                std::string inviteAll(translationInviteAll[current_language]);
                inviteAll.append("##PopupInviteAllFriends");
                if (ImGui::Button(inviteAll.c_str())) { // if btn clicked
                    invite_all_friends_clicked = true;
                }
            }

            if (ImGuiHelper_BeginListBox("##label", friends.size())) {
                std::for_each(friends.begin(), friends.end(), [this](std::pair<Friend const, friend_window_state> &i) {
                    ImGui::PushID(i.second.id-base_friend_window_id+base_friend_item_id);

                    ImGui::Selectable(i.second.window_title.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
                    build_friend_context_menu(i.first, i.second);
                    if (ImGui::IsItemClicked() && ImGui::IsMouseDoubleClicked(0)) {
                        i.second.window_state |= window_state_show;
                    }
                    ImGui::PopID();

                    build_friend_window(i.first, i.second);
                });
                ImGui::EndListBox();
            }
        }

        // user clicked on "show achievements" button
        if (show_achievements && achievements.size()) {
            ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 32, ImGui::GetFontSize() * 32), ImVec2(8192, 8192));
            ImGui::SetNextWindowBgAlpha(0.9f);
            bool show = show_achievements;
            if (ImGui::Begin(translationAchievementWindow[current_language], &show)) {
                ImGui::Text("%s", translationListOfAchievements[current_language]);
                ImGui::BeginChild(translationAchievements[current_language]);
                for (auto & x : achievements) {
                    bool achieved = x.achieved;
                    bool hidden = x.hidden && !achieved;

                    try_load_ach_icon(x);
                    try_load_ach_gray_icon(x);

                    ImGui::Separator();

                    bool could_create_ach_table_entry = false;
                    if (!x.icon.expired() || !x.icon_gray.expired()) {
                        if (ImGui::BeginTable(x.title.c_str(), 2)) {
                            could_create_ach_table_entry = true;

                            ImGui::TableSetupColumn("imgui_table_image", ImGuiTableColumnFlags_WidthFixed, settings->overlay_appearance.icon_size);
                            ImGui::TableSetupColumn("imgui_table_text");
                            ImGui::TableNextRow(ImGuiTableRowFlags_None, settings->overlay_appearance.icon_size);

                            ImGui::TableSetColumnIndex(0);
                            if (achieved) {
                                if (!x.icon.expired()) {
                                    ImGui::Image(
                                        (ImTextureID)*x.icon.lock().get(),
                                        ImVec2(settings->overlay_appearance.icon_size, settings->overlay_appearance.icon_size)
                                    );
                                }
                            } else {
                                if (!x.icon_gray.expired()) {
                                    ImGui::Image(
                                        (ImTextureID)*x.icon_gray.lock().get(),
                                        ImVec2(settings->overlay_appearance.icon_size, settings->overlay_appearance.icon_size)
                                    );
                                }
                            }

                            ImGui::TableSetColumnIndex(1);
                            // the next column is the achievement text below
                        }
                    }
                    
                    // we want to display the ach text regardless the icons were displayed or not
                    ImGui::Text("%s", x.title.c_str());

                    if (hidden) {
                        ImGui::Text("%s", translationHiddenAchievement[current_language]);
                    } else {
                        ImGui::TextWrapped("%s", x.description.c_str());
                    }

                    if (achieved) {
                        char buffer[80] = {};
                        time_t unlock_time = (time_t)x.unlock_time;
                        std::strftime(buffer, 80, "%Y-%m-%d at %H:%M:%S", std::localtime(&unlock_time));

                        ImGui::TextColored(ImVec4(0, 255, 0, 255), translationAchievedOn[current_language], buffer);
                    } else {
                        ImGui::TextColored(ImVec4(255, 0, 0, 255), "%s", translationNotAchieved[current_language]);
                    }

                    if (could_create_ach_table_entry) ImGui::EndTable();

                    ImGui::Separator();
                }
                ImGui::EndChild();
            }
            
            ImGui::End();
            show_achievements = show;
        }

        // user clicked on "settings" button
        if (show_settings) {
            ImGui::SetNextWindowBgAlpha(0.9f);
            if (ImGui::Begin(translationGlobalSettingsWindow[current_language], &show_settings)) {
                ImGui::Text("%s", translationGlobalSettingsWindowDescription[current_language]);

                ImGui::Separator();

                ImGui::Text("%s", translationUsername[current_language]);
                ImGui::SameLine();
                ImGui::InputText("##username", username_text, sizeof(username_text), disable_user_input ? ImGuiInputTextFlags_ReadOnly : 0);

                ImGui::Separator();

                ImGui::Text("%s", translationLanguage[current_language]);
                ImGui::ListBox("##language", &current_language, valid_languages, sizeof(valid_languages) / sizeof(valid_languages[0]), 7);
                ImGui::Text(translationSelectedLanguage[current_language], valid_languages[current_language]);

                ImGui::Separator();

                if (!disable_user_input) {
                    ImGui::Text("%s", translationRestartTheGameToApply[current_language]);
                    if (ImGui::Button(translationSave[current_language])) {
                        save_settings = true;
                        show_settings = false;
                    }
                } else {
                    ImGui::TextColored(ImVec4(255, 0, 0, 255), "%s", translationWarningWarningWarning[current_language]);
                    ImGui::TextWrapped("%s", translationWarningDescription1[current_language]);
                    ImGui::TextColored(ImVec4(255, 0, 0, 255), "%s", translationWarningWarningWarning[current_language]);
                }
            }

            ImGui::End();
        }

        // we have a url to open/display
        if (show_url.size()) {
            std::string url = show_url;
            bool show = true;
            ImGui::SetNextWindowBgAlpha(0.9f);
            if (ImGui::Begin(URL_WINDOW_NAME, &show)) {
                ImGui::Text("%s", translationSteamOverlayURL[current_language]);
                ImGui::Spacing();
                ImGui::PushItemWidth(ImGui::CalcTextSize(url.c_str()).x + 20);
                ImGui::InputText("##url_copy", (char *)url.data(), url.size(), ImGuiInputTextFlags_ReadOnly);
                ImGui::PopItemWidth();
                ImGui::Spacing();
                if (ImGui::Button(translationClose[current_language]) || !show)
                    show_url = "";
                // ImGui::SetWindowSize(ImVec2(ImGui::CalcTextSize(url.c_str()).x + 10, 0));
            }

            ImGui::End();
        }

        bool show_warning = warn_local_save || warn_forced_setting || warn_bad_appid;
        if (show_warning) {
            ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 32, ImGui::GetFontSize() * 32), ImVec2(8192, 8192));
            ImGui::SetNextWindowFocus();
            ImGui::SetNextWindowBgAlpha(0.9f);
            if (ImGui::Begin(translationWarning[current_language], &show_warning)) {
                if (warn_bad_appid) {
                    ImGui::TextColored(ImVec4(255, 0, 0, 255), "%s", translationWarningWarningWarning[current_language]);
                    ImGui::TextWrapped("%s", translationWarningDescription2[current_language]);
                    ImGui::TextColored(ImVec4(255, 0, 0, 255), "%s", translationWarningWarningWarning[current_language]);
                }
                if (warn_local_save) {
                    ImGui::TextColored(ImVec4(255, 0, 0, 255), "%s", translationWarningWarningWarning[current_language]);
                    ImGui::TextWrapped("%s", translationWarningDescription3[current_language]);
                    ImGui::TextColored(ImVec4(255, 0, 0, 255), "%s", translationWarningWarningWarning[current_language]);
                }
                if (warn_forced_setting) {
                    ImGui::TextColored(ImVec4(255, 0, 0, 255), "%s", translationWarningWarningWarning[current_language]);
                    ImGui::TextWrapped("%s", translationWarningDescription4[current_language]);
                    ImGui::TextColored(ImVec4(255, 0, 0, 255), "%s", translationWarningWarningWarning[current_language]);
                }
            }

            ImGui::End();
            if (!show_warning) {
                warn_local_save = warn_forced_setting = false;
            }
        }
    }

    ImGui::End();

    ImGui::PopFont();

    if (!show) ShowOverlay(false);

}

void Steam_Overlay::networking_msg_received(Common_Message *msg)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    
    if (!Ready()) return;

    if (msg->has_steam_messages()) {
        Friend frd;
        frd.set_id(msg->source_id());
        auto friend_info = friends.find(frd);
        if (friend_info != friends.end()) {
            Steam_Messages const& steam_message = msg->steam_messages();
            // Change color to cyan for friend
            friend_info->second.chat_history.append(friend_info->first.name() + ": " + steam_message.message()).append("\n", 1);
            if (!(friend_info->second.window_state & window_state_show)) {
                friend_info->second.window_state |= window_state_need_attention;
            }

            add_chat_message_notification(friend_info->first.name() + ": " + steam_message.message());
            notify_sound_user_invite(friend_info->second);
        }
    }
}

void Steam_Overlay::steam_run_callback()
{
    renderer_detector_delay_thread();
    renderer_hook_init_thread();

    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    
    if (!Ready()) return;

    if (overlay_state_changed) {
        overlay_state_changed = false;

        GameOverlayActivated_t data{};
        data.m_bActive = show_overlay;
        data.m_bUserInitiated = true;
        data.m_dwOverlayPID = 123;
        data.m_nAppID = settings->get_local_game_id().AppID();
        callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
    }

    Steam_Friends* steamFriends = get_steam_client()->steam_friends;
    Steam_Matchmaking* steamMatchmaking = get_steam_client()->steam_matchmaking;

    if (save_settings) {
        save_settings = false;

        const char *language_text = valid_languages[current_language];
        save_global_settings(get_steam_client()->local_storage, username_text, language_text);
        get_steam_client()->settings_client->set_local_name(username_text);
        get_steam_client()->settings_server->set_local_name(username_text);
        get_steam_client()->settings_client->set_language(language_text);
        get_steam_client()->settings_server->set_language(language_text);
        steamFriends->resend_friend_data();
    }

    i_have_lobby = got_lobby();
    std::for_each(friends.begin(), friends.end(), [this](std::pair<Friend const, friend_window_state> &i)
    {
        i.second.joinable = is_friend_joinable(i);
    });

    while (!has_friend_action.empty()) {
        auto friend_info = friends.find(has_friend_action.front());
        if (friend_info != friends.end()) {
            uint64 friend_id = (uint64)friend_info->first.id();
            // The user clicked on "Send"
            if (friend_info->second.window_state & window_state_send_message) {
                char* input = friend_info->second.chat_input;
                char* end_input = input + strlen(input);
                char* printable_char = std::find_if(input, end_input, [](char c) { return std::isgraph(c); });

                // Check if the message contains something else than blanks
                if (printable_char != end_input) {
                    // Handle chat send
                    Common_Message msg;
                    Steam_Messages* steam_messages = new Steam_Messages;
                    steam_messages->set_type(Steam_Messages::FRIEND_CHAT);
                    steam_messages->set_message(friend_info->second.chat_input);
                    msg.set_allocated_steam_messages(steam_messages);
                    msg.set_source_id(settings->get_local_steam_id().ConvertToUint64());
                    msg.set_dest_id(friend_id);
                    network->sendTo(&msg, true);

                    friend_info->second.chat_history.append(get_steam_client()->settings_client->get_local_name()).append(": ").append(input).append("\n", 1);
                }
                *input = 0; // Reset the input field

                friend_info->second.window_state &= ~window_state_send_message;
            }
            // The user clicked on "Invite" (but invite all wasn't clicked)
            if (friend_info->second.window_state & window_state_invite) {
                invite_friend(friend_id, steamFriends, steamMatchmaking);
                
                friend_info->second.window_state &= ~window_state_invite;
            }
            // The user clicked on "Join"
            if (friend_info->second.window_state & window_state_join) {
                std::string connect = steamFriends->GetFriendRichPresence(friend_id, "connect");
                // The user got a lobby invite and accepted it
                if (friend_info->second.window_state & window_state_lobby_invite) {
                    GameLobbyJoinRequested_t data;
                    data.m_steamIDLobby.SetFromUint64(friend_info->second.lobbyId);
                    data.m_steamIDFriend.SetFromUint64(friend_id);
                    callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));

                    friend_info->second.window_state &= ~window_state_lobby_invite;
                } else {
                    // The user got a rich presence invite and accepted it
                    if (friend_info->second.window_state & window_state_rich_invite) {
                        GameRichPresenceJoinRequested_t data = {};
                        data.m_steamIDFriend.SetFromUint64(friend_id);
                        strncpy(data.m_rgchConnect, friend_info->second.connect, k_cchMaxRichPresenceValueLength - 1);
                        callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
                        
                        friend_info->second.window_state &= ~window_state_rich_invite;
                    } else if (connect.length() > 0) {
                        GameRichPresenceJoinRequested_t data = {};
                        data.m_steamIDFriend.SetFromUint64(friend_id);
                        strncpy(data.m_rgchConnect, connect.c_str(), k_cchMaxRichPresenceValueLength - 1);
                        callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
                    }

                    //Not sure about this but it fixes sonic racing transformed invites
                    FriendGameInfo_t friend_game_info = {};
                    steamFriends->GetFriendGamePlayed(friend_id, &friend_game_info);
                    uint64 lobby_id = friend_game_info.m_steamIDLobby.ConvertToUint64();
                    if (lobby_id) {
                        GameLobbyJoinRequested_t data;
                        data.m_steamIDLobby.SetFromUint64(lobby_id);
                        data.m_steamIDFriend.SetFromUint64(friend_id);
                        callbacks->addCBResult(data.k_iCallback, &data, sizeof(data));
                    }
                }
                
                friend_info->second.window_state &= ~window_state_join;
            }
        }
        has_friend_action.pop();
    }

    // if variable == true, then set it to false and return true (because state was changed) in that case
    bool yes_clicked = true;
    if (invite_all_friends_clicked.compare_exchange_weak(yes_clicked, false)) {
        PRINT_DEBUG("Steam_Overlay will send invitations to [%zu] friends if they're using the same app\n", friends.size());
        uint32 current_appid = settings->get_local_game_id().AppID();
        for (auto &fr : friends) {
            if (fr.first.appid() == current_appid) { // friend is playing the same game
                uint64 friend_id = (uint64)fr.first.id();
                invite_friend(friend_id, steamFriends, steamMatchmaking);
            }
        }
    }
}


void Steam_Overlay::SetupOverlay()
{
    PRINT_DEBUG("Steam_Overlay::SetupOverlay\n");
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (settings->disable_overlay) return;

    bool not_called_yet = false;
    if (setup_overlay_called.compare_exchange_weak(not_called_yet, true)) {
        setup_time = std::chrono::high_resolution_clock::now();
        
        // "HITMAN 3" fails if the detector was started later (after a delay)
        // so request the renderer detector immediately (the old behavior)
        if (!settings->overlay_hook_delay_sec) {
            renderer_detector_delay_thread();
        }
    }
}

void Steam_Overlay::UnSetupOverlay()
{
    PRINT_DEBUG("Steam_Overlay::UnSetupOverlay\n");
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    bool already_called = true;
    if (setup_overlay_called.compare_exchange_weak(already_called, false)) {
        is_ready = false;

        free_renderer_detector();

        if (_renderer) {
            // for some reason this gets triggered after the overlay instance has been destroyed
            // I assume because the game de-initializes DX later after closing Steam APIs
            // this hacky solution just sets it to an empty function
            _renderer->OverlayHookReady = [](InGameOverlay::OverlayHookState state) {};

            // stop internal frame processing & restore cursor
            allow_renderer_frame_processing(false, true);
            obscure_game_input(false);

            PRINT_DEBUG("Steam_Overlay::UnSetupOverlay will free any images resources\n");
            for (auto &ach : achievements) {
                if (!ach.icon.expired()) {
                    _renderer->ReleaseImageResource(ach.icon);
                    ach.icon.reset();
                }

                if (!ach.icon_gray.expired()) {
                    _renderer->ReleaseImageResource(ach.icon_gray);
                    ach.icon_gray.reset();
                }
            }

            _renderer = nullptr;
        }

    }
}

bool Steam_Overlay::Ready() const
{
    return is_ready && late_init_imgui && late_init_ach_icons;
}

bool Steam_Overlay::NeedPresent() const
{
    PRINT_DEBUG("Steam_Overlay::NeedPresent\n");
    return !settings->disable_overlay;
}

void Steam_Overlay::SetNotificationPosition(ENotificationPosition eNotificationPosition)
{
    PRINT_DEBUG("TODO Steam_Overlay::SetNotificationPosition %i\n", (int)eNotificationPosition);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    notif_position = eNotificationPosition;
}

void Steam_Overlay::SetNotificationInset(int nHorizontalInset, int nVerticalInset)
{
    PRINT_DEBUG("TODO Steam_Overlay::SetNotificationInset x=%i y=%i\n", nHorizontalInset, nVerticalInset);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);

    h_inset = nHorizontalInset;
    v_inset = nVerticalInset;
}

void Steam_Overlay::OpenOverlayInvite(CSteamID lobbyId)
{
    PRINT_DEBUG("TODO Steam_Overlay::OpenOverlayInvite %llu\n", lobbyId.ConvertToUint64());
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    ShowOverlay(true);
}

void Steam_Overlay::OpenOverlay(const char* pchDialog)
{
    PRINT_DEBUG("TODO Steam_Overlay::OpenOverlay '%s'\n", pchDialog);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;
    
    // TODO: Show pages depending on pchDialog
    if ((strncmp(pchDialog, "Friends", sizeof("Friends") - 1) == 0) && (settings->overlayAutoAcceptInvitesCount() > 0)) {
        PRINT_DEBUG("Steam_Overlay won't open overlay's friends list because some friends are defined in the auto accept list\n");
        add_auto_accept_invite_notification();
    } else {
        ShowOverlay(true);
    }
}

void Steam_Overlay::OpenOverlayWebpage(const char* pchURL)
{
    PRINT_DEBUG("TODO Steam_Overlay::OpenOverlayWebpage '%s'\n", pchURL);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;
    
    show_url = pchURL;
    ShowOverlay(true);
}

bool Steam_Overlay::ShowOverlay() const
{
    return show_overlay;
}

void Steam_Overlay::ShowOverlay(bool state)
{
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready() || show_overlay == state) return;

    show_overlay = state;
    overlay_state_changed = true;
    
    PRINT_DEBUG("Steam_Overlay::ShowOverlay %i\n", (int)state);
    
    Steam_Overlay::allow_renderer_frame_processing(state);
    Steam_Overlay::obscure_game_input(state);

}

void Steam_Overlay::SetLobbyInvite(Friend friendId, uint64 lobbyId)
{
    PRINT_DEBUG("Steam_Overlay::SetLobbyInvite " "%" PRIu64 " %llu\n", friendId.id(), lobbyId);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    auto i = friends.find(friendId);
    if (i != friends.end())
    {
        auto& frd = i->second;
        frd.lobbyId = lobbyId;
        frd.window_state |= window_state_lobby_invite;
        // Make sure don't have rich presence invite and a lobby invite (it should not happen but who knows)
        frd.window_state &= ~window_state_rich_invite;
        add_invite_notification(*i);
        notify_sound_user_invite(i->second);
    }
}

void Steam_Overlay::SetRichInvite(Friend friendId, const char* connect_str)
{
    PRINT_DEBUG("Steam_Overlay::SetRichInvite " "%" PRIu64 " '%s'\n", friendId.id(), connect_str);
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    auto i = friends.find(friendId);
    if (i != friends.end())
    {
        auto& frd = i->second;
        strncpy(frd.connect, connect_str, k_cchMaxRichPresenceValueLength - 1);
        frd.window_state |= window_state_rich_invite;
        // Make sure don't have rich presence invite and a lobby invite (it should not happen but who knows)
        frd.window_state &= ~window_state_lobby_invite;
        add_invite_notification(*i);
        notify_sound_user_invite(i->second);
    }
}

void Steam_Overlay::FriendConnect(Friend _friend)
{
    PRINT_DEBUG("Steam_Overlay::FriendConnect " "%" PRIu64 "\n", _friend.id());
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    // players connections might happen earlier before the overlay is ready
    // we don't want to miss them
    //if (!Ready()) return;
    
    int id = find_free_friend_id(friends);
    if (id != 0) {
        auto& item = friends[_friend];
        item.window_title = std::move(_friend.name() + translationPlaying[current_language] + std::to_string(_friend.appid()));
        item.window_state = window_state_none;
        item.id = id;
        memset(item.chat_input, 0, max_chat_len);
        item.joinable = false;
    } else {
        PRINT_DEBUG("Steam_Overlay::FriendConnect error no free id to create a friend window\n");
    }
}

void Steam_Overlay::FriendDisconnect(Friend _friend)
{
    PRINT_DEBUG("Steam_Overlay::FriendDisconnect " "%" PRIu64 "\n", _friend.id());
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    // players connections might happen earlier before the overlay is ready
    // we don't want to miss them
    //if (!Ready()) return;
    
    auto it = friends.find(_friend);
    if (it != friends.end())
        friends.erase(it);
}

// show a notification when the user unlocks an achievement
void Steam_Overlay::AddAchievementNotification(nlohmann::json const& ach)
{
    PRINT_DEBUG("Steam_Overlay::AddAchievementNotification\n");
    std::lock_guard<std::recursive_mutex> lock(overlay_mutex);
    if (!Ready()) return;

    // don't return early when disable_overlay_achievement_notification is true
    // otherwise when you open the achievements list/menu you won't see the new unlock status

    std::vector<Overlay_Achievement*> found_achs{};
    {
        std::lock_guard<std::recursive_mutex> lock2(global_mutex);

        std::string ach_name = ach.value("name", std::string());
        for (auto &a : achievements) {
            if (a.name == ach_name) {
                found_achs.push_back(&a);
                
                bool achieved = false;
                uint32 unlock_time = 0;
                get_steam_client()->steam_user_stats->GetAchievementAndUnlockTime(a.name.c_str(), &achieved, &unlock_time);
                a.achieved = achieved;
                a.unlock_time = unlock_time;
            }
        }
    }

    if (!settings->disable_overlay_achievement_notification) {
        for (auto found_ach : found_achs) {
            try_load_ach_icon(*found_ach);
            submit_notification(
                notification_type_achievement,
                ach.value("displayName", std::string()) + "\n" + ach.value("description", std::string()),
                {},
                found_ach->icon
            );
        }
        
        notify_sound_user_achievement();
    }
}

#endif
