//
// Copyleft 2012 RIME Developers
// License: GPLv3
//
// 2012-03-23 GONG Chen <chen.sst@gmail.com>
//
#include <fstream>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/foreach.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/scope_exit.hpp>
#include <rime/common.h>
#include <rime/deployer.h>
#include <rime/algo/dynamics.h>
#include <rime/algo/utilities.h>
#include <rime/dict/user_db.h>
#include <rime/dict/user_dictionary.h>
#include <rime/lever/user_dict_manager.h>

namespace fs = boost::filesystem;

namespace rime {

static bool IsUserDb(UserDb& db) {
  std::string db_type;
  return db.Fetch("\x01/db_type", &db_type) && db_type == "userdb";
}

static const std::string GetDbName(UserDb& db) {
  std::string name;
  if (!db.Fetch("\x01/db_name", &name))
    return name;
  boost::erase_last(name, ".kct");
  boost::erase_last(name, ".userdb");
  return name;
}

static const std::string GetUserId(UserDb& db) {
  std::string user_id("unknown");
  db.Fetch("\x01/user_id", &user_id);
  return user_id;
}

static TickCount GetTickCount(UserDb& db) {
  std::string tick;
  if (db.Fetch("\x01/tick", &tick)) {
    try {
      return boost::lexical_cast<TickCount>(tick);
    }
    catch (...) {
    }
  }
  return 1;
}

UserDictManager::UserDictManager(Deployer* deployer)
    : deployer_(deployer) {
  if (deployer) {
    path_ = deployer->user_data_dir;
  }
}

void UserDictManager::GetUserDictList(UserDictList* user_dict_list) {
  if (!user_dict_list) return;
  user_dict_list->clear();
  if (!fs::exists(path_) || !fs::is_directory(path_)) {
    LOG(INFO) << "directory '" << path_.string() << "' does not exist.";
    return;
  }
  fs::directory_iterator it(path_);
  fs::directory_iterator end;
  for (; it != end; ++it) {
    std::string name(it->path().filename().string());
    if (boost::ends_with(name, ".userdb.kct")) {
      boost::erase_last(name, ".userdb.kct");
      user_dict_list->push_back(name);
    }
  }
}

bool UserDictManager::Backup(const std::string& dict_name) {
  UserDb db(dict_name);
  if (!db.OpenReadOnly())
    return false;
  if (GetUserId(db) != deployer_->user_id) {
    LOG(INFO) << "user id not match; recreating metadata in " << dict_name;
    if (!db.Close() || !db.Open() || !db.CreateMetadata()) {
      LOG(ERROR) << "failed to recreate metadata in " << dict_name;
      return false;
    }
  }
  return db.Backup();
}

bool UserDictManager::Restore(const std::string& snapshot_file) {
  UserDb temp(".temp");
  if (temp.Exists())
    temp.Remove();
  if (!temp.Open())
    return false;
  BOOST_SCOPE_EXIT( (&temp) )
  {
    temp.Close();
    temp.Remove();
  } BOOST_SCOPE_EXIT_END
  if (!temp.Restore(snapshot_file))
    return false;
  if (!IsUserDb(temp))
    return false;
  std::string db_name(GetDbName(temp));
  if (db_name.empty())
    return false;
  UserDb dest(db_name);
  if (!dest.Open())
    return false;
  BOOST_SCOPE_EXIT( (&dest) )
  {
    dest.Close();
  } BOOST_SCOPE_EXIT_END
  LOG(INFO) << "merging '" << snapshot_file
            << "' from " << GetUserId(temp)
            << " into userdb '" << db_name << "'...";
  TickCount tick_left = GetTickCount(dest);
  TickCount tick_right = GetTickCount(temp);
  TickCount tick_max = (std::max)(tick_left, tick_right);
  shared_ptr<TreeDbAccessor> a = temp.Query("");
  std::string key, left, right;
  int num_entries = 0;
  while (a->GetNextRecord(&key, &right)) {
    if (boost::starts_with(key, "\x01/"))  // skip metadata
      continue;
    size_t tab_pos = key.find('\t');
    if (tab_pos == 0 || tab_pos == std::string::npos)
      continue;
    // fix invalid keys created by a buggy version of Import()
    if (key[tab_pos - 1] != ' ')
      key.insert(tab_pos, 1, ' ');
    int c = 0;
    double d = 0.0;
    TickCount t = 0;
    UserDictionary::UnpackValues(right, &c, &d, &t);
    if (t < tick_right)
      d = algo::formula_d(0, (double)tick_right, d, (double)t);
    if (dest.Fetch(key, &left)) {
      int c0 = 0;
      double d0 = 0.0;
      TickCount t0 = 0;
      UserDictionary::UnpackValues(left, &c0, &d0, &t0);
      if (t0 < tick_left)
        d0 = algo::formula_d(0, (double)tick_left, d0, (double)t0);
      c = (std::max)(c, c0);
      d = (std::max)(d, d0);
    }
    right = boost::str(boost::format("c=%1% d=%2% t=%3%") %
                       c % d % tick_max);
    if (dest.Update(key, right))
      ++num_entries;
  }
  if (num_entries > 0) {
    try {
      dest.Update("\x01/tick", boost::lexical_cast<std::string>(tick_max));
      dest.Update("\x01/user_id", deployer_->user_id);
    }
    catch (...) {
      LOG(WARNING) << "failed to update tick count.";
    }
  }
  LOG(INFO) << "total " << num_entries << " entries imported, tick = "
            << tick_max;
  return true;
}

int UserDictManager::Export(const std::string& dict_name,
                            const std::string& text_file) {
  UserDb db(dict_name);
  if (!db.OpenReadOnly())
    return -1;
  BOOST_SCOPE_EXIT( (&db) )
  {
    db.Close();
  } BOOST_SCOPE_EXIT_END
  if (!IsUserDb(db))
    return -1;
  std::ofstream fout(text_file.c_str());
  fout << "# Rime user dictionary export" << std::endl
       << "# db_name: " << GetDbName(db) << std::endl
       << "# user_id: " << GetUserId(db) << std::endl
       << "# commits: " << GetTickCount(db) << std::endl
       << std::endl;
  std::string key, value;
  std::vector<std::string> row;
  int num_entries = 0;
  shared_ptr<UserDbAccessor> a = db.Query("");
  while (a->GetNextRecord(&key, &value)) {
    if (boost::starts_with(key, "\x01/"))  // skip metadata
      continue;
    boost::algorithm::split(row, key,
                            boost::algorithm::is_any_of("\t"));
    if (row.size() != 2 ||
        row[0].empty() || row[1].empty())
      continue;
    boost::algorithm::trim(row[0]);
    int c = 0;
    double d = 0.0;
    TickCount t = 0;
    UserDictionary::UnpackValues(value, &c, &d, &t);
    if (c < 0)  // deleted entry
      continue;
    fout << row[1] << "\t" << row[0] << "\t" << c << std::endl;
    ++num_entries;
  }
  fout.close();
  return num_entries;
}

int UserDictManager::Import(const std::string& dict_name,
                            const std::string& text_file) {
  UserDb db(dict_name);
  if (!db.Open())
    return -1;
  BOOST_SCOPE_EXIT( (&db) )
  {
    db.Close();
  } BOOST_SCOPE_EXIT_END
  if (!IsUserDb(db))
    return -1;
  std::ifstream fin(text_file.c_str());
  std::string line, key, value;
  int num_entries = 0;
  while (getline(fin, line)) {
    // skip empty lines and comments
    if (line.empty() || line[0] == '#') continue;
    // read a dict entry
    std::vector<std::string> row;
    boost::algorithm::split(row, line,
                            boost::algorithm::is_any_of("\t"));
    if (row.size() < 2 ||
        row[0].empty() || row[1].empty()) {
      LOG(WARNING) << "invalid entry at #" << num_entries << ".";
      continue;
    }
    boost::algorithm::trim(row[1]);
    if (!row[1].empty()) {
      std::vector<std::string> syllables;
      boost::algorithm::split(syllables, row[1],
                              boost::algorithm::is_any_of(" "),
                              boost::algorithm::token_compress_on);
      row[1] = boost::algorithm::join(syllables, " ");
    }
    key = row[1] + " \t" + row[0];
    int commits = 0;
    if (row.size() >= 3 && !row[2].empty()) {
      try {
        commits = boost::lexical_cast<int>(row[2]);
      }
      catch (...) {
      }
    }
    int c = 0;
    double d = 0.0;
    TickCount t = 0;
    if (db.Fetch(key, &value))
      UserDictionary::UnpackValues(value, &c, &d, &t);
    if (commits > 0)
      c = (std::max)(commits, c);
    else if (commits < 0)  // mark as deleted
      c = commits;
    value = boost::str(boost::format("c=%1% d=%2% t=%3%") %
                       c % d % t);
    if (db.Update(key, value))
      ++num_entries;
  }
  fin.close();
  return num_entries;
}

bool UserDictManager::UpgradeUserDict(const std::string& dict_name) {
  UserDb db(dict_name);
  if (!db.OpenReadOnly())
    return false;
  if (!IsUserDb(db))
    return false;
  std::string db_creator_version;
  db.Fetch("\x01/rime_version", &db_creator_version);
  if (CompareVersionString(db_creator_version, "0.9.7") < 0) {
    // fix invalid keys created by a buggy version of Import()
    LOG(INFO) << "upgrading user dict '" << dict_name << "'.";
    fs::path snapshot_file(deployer_->user_data_sync_dir());
    snapshot_file /= (db.name() + ".snapshot");
    return db.Backup() &&
        db.Close() &&
        db.Remove() &&
        Restore(snapshot_file.string());
  }
  return true;
}

bool UserDictManager::Synchronize(const std::string& dict_name) {
  LOG(INFO) << "synchronize user dict '" << dict_name << "'.";
  bool success = true;
  fs::path sync_dir(deployer_->sync_dir);
  if (!fs::exists(sync_dir)) {
    boost::system::error_code ec;
    if (!fs::create_directories(sync_dir, ec)) {
      LOG(ERROR) << "error creating directory '" << sync_dir.string() << "'.";
      return false;
    }
  }
  fs::directory_iterator it(sync_dir);
  fs::directory_iterator end;
  const std::string snapshot_file(dict_name + ".userdb.kct.snapshot");
  for (; it != end; ++it) {
    if (!fs::is_directory(it->path()))
      continue;
    fs::path file_path = it->path() / snapshot_file;
    if (fs::exists(file_path)) {
      LOG(INFO) << "merging snapshot file: " << file_path.string();
      if (!Restore(file_path.string())) {
        LOG(ERROR) << "failed to merge snapshot file: " << file_path.string();
        success = false;
      }
    }
  }
  if (!Backup(dict_name)) {
    LOG(ERROR) << "error backing up user dict '" << dict_name << "'.";
    success = false;
  }
  return success;
}

bool UserDictManager::SynchronizeAll() {
  UserDictList user_dicts;
  GetUserDictList(&user_dicts);
  LOG(INFO) << "synchronizing " << user_dicts.size() << " user dicts.";
  BOOST_FOREACH(const std::string& dict_name, user_dicts) {
    if (!Synchronize(dict_name))
      return false;
  }
  return true;
}

}  // namespace rime
