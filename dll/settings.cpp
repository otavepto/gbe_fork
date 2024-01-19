/* Copyright (C) 2019 Mr Goldberg
   This file is part of the Goldberg Emulator

   The Goldberg Emulator is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   The Goldberg Emulator is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the Goldberg Emulator; if not, see
   <http://www.gnu.org/licenses/>.  */

#include "dll/settings.h"


std::string Settings::sanitize(std::string name)
{
    // https://github.com/microsoft/referencesource/blob/51cf7850defa8a17d815b4700b67116e3fa283c2/mscorlib/system/io/path.cs#L88C9-L89C1
    // https://github.com/microsoft/referencesource/blob/51cf7850defa8a17d815b4700b67116e3fa283c2/mscorlib/system/io/pathinternal.cs#L32
    static const char InvalidFileNameChars[] = {
        '\"', '<', '>', '|', '\0',
        (char)1, (char)2, (char)3, (char)4, (char)5, (char)6, (char)7, (char)8, (char)9, (char)10,
        (char)11, (char)12, (char)13, (char)14, (char)15, (char)16, (char)17, (char)18, (char)19, (char)20,
        (char)21, (char)22, (char)23, (char)24, (char)25, (char)26, (char)27, (char)28, (char)29, (char)30,
        (char)31,
        ':', '*', '?', /*'\\', '/',*/
    };

    // we have to use utf-32 because Windows (and probably Linux) allows some chars that need at least 32 bits,
    // such as this one (U+1F5FA) called "World Map": https://www.compart.com/en/unicode/U+1F5FA
    // utf-16 encoding for these characters require 2 ushort, but we would like to iterate
    // over all chars in a linear fashion
    std::u32string unicode_name;
    utf8::utf8to32(
        name.begin(),
        utf8::find_invalid(name.begin(), name.end()), // returns an iterator pointing to the first invalid octet
        std::back_inserter(unicode_name));
    
    unicode_name.erase(std::remove(unicode_name.begin(), unicode_name.end(), '\n'), unicode_name.end());
    unicode_name.erase(std::remove(unicode_name.begin(), unicode_name.end(), '\r'), unicode_name.end());

    auto InvalidFileNameChars_last_it = std::end(InvalidFileNameChars);
    for (auto& i : unicode_name)
    {
        auto found_it = std::find(std::begin(InvalidFileNameChars), InvalidFileNameChars_last_it, i);
        if (found_it != InvalidFileNameChars_last_it) { // if illegal
            i = ' ';
        }
    }

    std::string res;
    utf8::utf32to8(unicode_name.begin(), unicode_name.end(), std::back_inserter(res));
    return res;
}

Settings::Settings(CSteamID steam_id, CGameID game_id, std::string name, std::string language, bool offline)
{
    this->steam_id = steam_id;
    this->game_id = game_id;
    this->name = sanitize(name);
    if (this->name.size() == 0) {
        this->name = "  ";
    }

    if (this->name.size() == 1) {
        this->name = this->name + " ";
    }

    auto lang = sanitize(language);
    std::transform(lang.begin(), lang.end(), lang.begin(), ::tolower);
    lang.erase(std::remove(lang.begin(), lang.end(), ' '), lang.end());
    this->language = lang;
    this->lobby_id = k_steamIDNil;
    this->unlockAllDLCs = true;

    this->offline = offline;
    this->create_unknown_leaderboards = true;
}

CSteamID Settings::get_local_steam_id()
{
    return steam_id;
}

CGameID Settings::get_local_game_id()
{
    return game_id;
}

const char *Settings::get_local_name()
{
    return name.c_str();
}

const char *Settings::get_language()
{
    return language.c_str();
}

void Settings::set_local_name(char *name)
{
    this->name = name;
}

void Settings::set_language(char *language)
{
    this->language = language;
}

void Settings::set_game_id(CGameID game_id)
{
    this->game_id = game_id;
}

void Settings::set_lobby(CSteamID lobby_id)
{
    this->lobby_id = lobby_id;
}

CSteamID Settings::get_lobby()
{
    return this->lobby_id;
}

void Settings::unlockAllDLC(bool value)
{
    this->unlockAllDLCs = value;
}

void Settings::addDLC(AppId_t appID, std::string name, bool available)
{
    auto f = std::find_if(DLCs.begin(), DLCs.end(), [&appID](DLC_entry const& item) { return item.appID == appID; });
    if (DLCs.end() != f) {
        f->name = name;
        f->available = available;
        return;
    }

    DLC_entry new_entry;
    new_entry.appID = appID;
    new_entry.name = name;
    new_entry.available = available;
    DLCs.push_back(new_entry);
}

void Settings::addMod(PublishedFileId_t id, std::string title, std::string path)
{
    auto f = std::find_if(mods.begin(), mods.end(), [&id](Mod_entry const& item) { return item.id == id; });
    if (mods.end() != f) {
        f->title = title;
        f->path = path;
        return;
    }

    Mod_entry new_entry;
    new_entry.id = id;
    new_entry.title = title;
    new_entry.path = path;
    mods.push_back(new_entry);
}

void Settings::addModDetails(PublishedFileId_t id, Mod_entry details)
{
    auto f = std::find_if(mods.begin(), mods.end(), [&id](Mod_entry const& item) { return item.id == id; });
    if (f != mods.end()) {
        f->previewURL = details.previewURL;
        f->fileType = details.fileType;
        f->description = details.description;
        f->steamIDOwner = details.steamIDOwner;
        f->timeCreated = details.timeCreated;
        f->timeUpdated = details.timeUpdated;
        f->timeAddedToUserList = details.timeAddedToUserList;
        f->visibility = details.visibility;
        f->banned = details.banned;
        f->acceptedForUse = details.acceptedForUse;
        f->tagsTruncated = details.tagsTruncated;
        f->tags = details.tags;
        // - should we set the handles here instead of Invalid?
        f->handleFile = details.handleFile;
        f->handlePreviewFile = details.handlePreviewFile;
        //  -
        f->primaryFileName = details.primaryFileName;
        f->primaryFileSize = details.primaryFileSize;
        f->previewFileName = details.previewFileName;
        f->previewFileSize = details.previewFileSize;
        f->workshopItemURL = details.workshopItemURL;
        f->votesUp = details.votesUp;
        f->votesDown = details.votesDown;
        f->score = details.score;
        f->numChildren = details.numChildren;
    }
}

Mod_entry Settings::getMod(PublishedFileId_t id)
{
    auto f = std::find_if(mods.begin(), mods.end(), [&id](Mod_entry const& item) { return item.id == id; });
    if (mods.end() != f) {
        return *f;
    }

    return Mod_entry();
}

bool Settings::isModInstalled(PublishedFileId_t id)
{
    auto f = std::find_if(mods.begin(), mods.end(), [&id](Mod_entry const& item) { return item.id == id; });
    if (mods.end() != f) {
        return true;
    }

    return false;
}

std::set<PublishedFileId_t> Settings::modSet()
{
    std::set<PublishedFileId_t> ret_set;

    for (auto & m: mods) {
        ret_set.insert(m.id);
    }

    return ret_set;
}

unsigned int Settings::DLCCount()
{
    return this->DLCs.size();
}

bool Settings::hasDLC(AppId_t appID)
{
    if (this->unlockAllDLCs) return true;

    auto f = std::find_if(DLCs.begin(), DLCs.end(), [&appID](DLC_entry const& item) { return item.appID == appID; });
    if (DLCs.end() == f)
        return false;

    return f->available;
}

bool Settings::getDLC(unsigned int index, AppId_t &appID, bool &available, std::string &name)
{
    if (index >= DLCs.size()) return false;

    appID = DLCs[index].appID;
    available = DLCs[index].available;
    name = DLCs[index].name;
    return true;
}

bool Settings::allDLCUnlocked() const
{
    return this->unlockAllDLCs;
}

void Settings::setAppInstallPath(AppId_t appID, std::string path)
{
    app_paths[appID] = path;
}

std::string Settings::getAppInstallPath(AppId_t appID)
{
    return app_paths[appID];
}

void Settings::setLeaderboard(std::string leaderboard, enum ELeaderboardSortMethod sort_method, enum ELeaderboardDisplayType display_type)
{
    Leaderboard_config leader;
    leader.sort_method = sort_method;
    leader.display_type = display_type;

    leaderboards[leaderboard] = leader;
}

int Settings::add_image(std::string data, uint32 width, uint32 height)
{
    int last = images.size() + 1;
    struct Image_Data dt;
    dt.width = width;
    dt.height = height;
    dt.data = data;
    images[last] = dt;
    return last;
}

bool Settings::appIsInstalled(AppId_t appID)
{
    if (this->assume_any_app_installed) return true;

    auto f = std::find_if(installed_app_ids.begin(), installed_app_ids.end(), [&appID](AppId_t const& item) { return item == appID; });
    if (installed_app_ids.end() == f)
        return false;

    return true;
}
