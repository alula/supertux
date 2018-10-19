//  SuperTux
//  Copyright (C) 2018 Ingo Ruhnke <grumbel@gmail.com>
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "worldmap/worldmap_parser.hpp"

#include <physfs.h>

#include "object/background.hpp"
#include "object/decal.hpp"
#include "object/tilemap.hpp"
#include "physfs/physfs_file_system.hpp"
#include "supertux/tile_manager.hpp"
#include "util/file_system.hpp"
#include "util/log.hpp"
#include "util/reader.hpp"
#include "util/reader_document.hpp"
#include "util/reader_mapping.hpp"
#include "util/reader_object.hpp"
#include "worldmap/level_tile.hpp"
#include "worldmap/spawn_point.hpp"
#include "worldmap/special_tile.hpp"
#include "worldmap/sprite_change.hpp"
#include "worldmap/teleporter.hpp"
#include "worldmap/tux.hpp"
#include "worldmap/worldmap.hpp"
#include "worldmap/worldmap_parser.hpp"
#include "worldmap/worldmap_screen.hpp"

namespace worldmap {

WorldMapParser::WorldMapParser(WorldMap& worldmap) :
  m_worldmap(worldmap)
{
}

void
WorldMapParser::load_worldmap(const std::string& filename)
{
  m_worldmap.m_map_filename = filename;
  m_worldmap.m_levels_path = FileSystem::dirname(m_worldmap.m_map_filename);

  try {
    register_translation_directory(m_worldmap.m_map_filename);
    auto doc = ReaderDocument::from_file(m_worldmap.m_map_filename);
    auto root = doc.get_root();

    if(root.get_name() != "supertux-level")
      throw std::runtime_error("file isn't a supertux-level file.");

    auto level_ = root.get_mapping();

    level_.get("name", m_worldmap.m_name);

    std::string tileset_name;
    if(level_.get("tileset", tileset_name)) {
      if(m_worldmap.m_tileset != nullptr) {
        log_warning << "multiple tilesets specified in level_" << std::endl;
      } else {
        m_worldmap.m_tileset = TileManager::current()->get_tileset(tileset_name);
      }
    }
    /* load default tileset */
    if(m_worldmap.m_tileset == nullptr) {
      m_worldmap.m_tileset = TileManager::current()->get_tileset("images/worldmap.strf");
    }

    boost::optional<ReaderMapping> sector;
    if(!level_.get("sector", sector)) {
      throw std::runtime_error("No sector specified in worldmap file.");
    } else {
      auto iter = sector->get_iter();
      while(iter.next()) {
        if(iter.get_key() == "tilemap") {
          m_worldmap.add_object(std::make_shared<TileMap>(m_worldmap.m_tileset, iter.as_mapping()));
        } else if(iter.get_key() == "background") {
          m_worldmap.add_object(std::make_shared<Background>(iter.as_mapping()));
        } else if(iter.get_key() == "music") {
          iter.get(m_worldmap.m_music);
        } else if(iter.get_key() == "init-script") {
          iter.get(m_worldmap.m_init_script);
        } else if(iter.get_key() == "worldmap-spawnpoint") {
          std::unique_ptr<SpawnPoint> sp(new SpawnPoint(iter.as_mapping()));
          m_worldmap.m_spawn_points.push_back(std::move(sp));
        } else if(iter.get_key() == "level") {
          auto level = std::make_shared<LevelTile>(m_worldmap.m_levels_path, iter.as_mapping());
          load_level_information(*level.get());
          m_worldmap.m_levels.push_back(level.get());
          m_worldmap.add_object(level);
        } else if(iter.get_key() == "special-tile") {
          auto special_tile = std::make_shared<SpecialTile>(iter.as_mapping());
          m_worldmap.m_special_tiles.push_back(special_tile.get());
          m_worldmap.add_object(special_tile);
        } else if(iter.get_key() == "sprite-change") {
          auto sprite_change = std::make_shared<SpriteChange>(iter.as_mapping());
          m_worldmap.m_sprite_changes.push_back(sprite_change.get());
          m_worldmap.add_object(sprite_change);
        } else if(iter.get_key() == "teleporter") {
          auto teleporter = std::make_shared<Teleporter>(iter.as_mapping());
          m_worldmap.m_teleporters.push_back(teleporter.get());
          m_worldmap.add_object(teleporter);
        } else if(iter.get_key() == "decal") {
          auto decal = std::make_shared<Decal>(iter.as_mapping());
          m_worldmap.add_object(decal);
        } else if(iter.get_key() == "ambient-light") {
          std::vector<float> vColor;
          bool hasColor = sector->get( "ambient-light", vColor );
          if(vColor.size() < 3 || !hasColor) {
            log_warning << "(ambient-light) requires a color as argument" << std::endl;
          } else {
            m_worldmap.m_ambient_light = Color( vColor );
          }
        } else if(iter.get_key() == "name") {
          // skip
        } else {
          log_warning << "Unknown token '" << iter.get_key() << "' in worldmap" << std::endl;
        }
      }
    }

    m_worldmap.update_game_objects();

    if (m_worldmap.get_solid_tilemaps().empty())
      throw std::runtime_error("No solid tilemap specified");

    m_worldmap.move_to_spawnpoint("main");

  } catch(std::exception& e) {
    std::stringstream msg;
    msg << "Problem when parsing worldmap '" << m_worldmap.m_map_filename << "': " <<
      e.what();
    throw std::runtime_error(msg.str());
  }
}

void
WorldMapParser::load_level_information(LevelTile& level)
{
  /** get special_tile's title */
  level.title = _("<no title>");
  level.target_time = 0.0f;

  try {
    std::string filename = m_worldmap.m_levels_path + level.get_name();

    if (m_worldmap.m_levels_path == "./")
      filename = level.get_name();

    if (!PHYSFS_exists(filename.c_str()))
    {
      log_warning << "Level file '" << filename << "' does not exist. Skipping." << std::endl;
      return;
    }
    if (PhysFSFileSystem::is_directory(filename))
    {
      log_warning << "Level file '" << filename << "' is a directory. Skipping." << std::endl;
      return;
    }

    register_translation_directory(filename);
    auto doc = ReaderDocument::from_file(filename);
    auto root = doc.get_root();
    if(root.get_name() != "supertux-level") {
      return;
    } else {
      auto level_lisp = root.get_mapping();
      level_lisp.get("name", level.title);
      level_lisp.get("target-time", level.target_time);
    }
  } catch(std::exception& e) {
    log_warning << "Problem when reading level information: " << e.what() << std::endl;
    return;
  }
}

} // namespace worldmap

/* EOF */