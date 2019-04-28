// git_cache.h
#pragma once

#include "bisect_first_match.h"
#include "call_git.h"
#include "error.h"
#include "sha1_pool.h"
#include "split2monodb.h"

namespace {
struct fparent_type {
  sha1_ref commit;
  long long ct = -1;
  int index = -1;
};
struct commit_type {
  sha1_ref commit;
  sha1_ref tree;
  sha1_ref *parents = nullptr;
  int num_parents = 0;
};

struct index_range {
  int first = -1;
  unsigned count = 0;
};
struct commit_source {
  index_range commits;
  int dir_index = -1;
  bool is_root = false;
};

struct dir_mask {
  static constexpr const int max_size = 64;
  std::bitset<max_size> bits;

  bool test(int i) const { return bits.test(i); }
  void reset(int i) { bits.reset(i); }
  void set(int i, bool value = true) { bits.set(i, value); }
};

struct dir_type {
  const char *name = nullptr;
  int source_index = -1;
  sha1_ref head;

  explicit dir_type(const char *name) : name(name) {}
};
struct dir_list {
  std::vector<dir_type> list;
  dir_mask active_dirs;

  int add_dir(const char *name, bool &is_new, int &d);
  int lookup_dir(const char *name, bool &found);
  void set_head(int d, sha1_ref head) {
    list[d].head = head;
    if (head)
      active_dirs.set(d);
  }
};

struct git_tree {
  struct item_type {
    sha1_ref sha1;
    const char *name = nullptr;
    bool is_tree = false;
    bool is_exec = false;
  };
  sha1_ref sha1;
  item_type *items = nullptr;
  int num_items = 0;
};
struct git_cache {
  git_cache(split2monodb &db, mmapped_file &svn2git, sha1_pool &pool,
            dir_list &dirs)
      : db(db), svn2git(svn2git), pool(pool), dirs(dirs) {}
  void note_commit_tree(sha1_ref commit, sha1_ref tree);
  void note_mono(sha1_ref split, sha1_ref mono);
  void note_rev(sha1_ref commit, int rev);
  void note_tree(const git_tree &tree);
  int lookup_commit_tree(sha1_ref commit, sha1_ref &tree) const;
  int lookup_mono(sha1_ref split, sha1_ref &mono) const;
  int lookup_rev(sha1_ref commit, int &rev) const;
  int lookup_tree(git_tree &tree) const;
  int get_commit_tree(sha1_ref commit, sha1_ref &tree);
  int get_rev(sha1_ref commit, int &rev);
  int get_mono(sha1_ref split, sha1_ref &mono);
  int set_mono(sha1_ref split, sha1_ref mono);
  int ls_tree(git_tree &tree);
  int mktree(git_tree &tree);

  const char *make_name(const char *name, size_t len);
  git_tree::item_type *make_items(git_tree::item_type *first,
                                  git_tree::item_type *last);

  struct commit_tree_buffers {
    std::string cn, cd, ce;
    std::string an, ad, ae;
    std::vector<textual_sha1> parents;
    std::vector<const char *> args;
    std::string message;
  };
  int commit_tree(sha1_ref base_commit, const char *dir, sha1_ref tree,
                  const std::vector<sha1_ref> &parents, sha1_ref &commit,
                  commit_tree_buffers &buffers);

  struct sha1_pair {
    sha1_ref key;
    sha1_ref value;
  };
  struct git_svn_base_rev {
    sha1_ref commit;
    int rev = -1;
  };

  static constexpr const int num_cache_bits = 16;

  git_tree trees[1u << num_cache_bits];
  sha1_pair commit_trees[1u << num_cache_bits];
  git_svn_base_rev revs[1u << num_cache_bits];
  sha1_pair monos[1u << num_cache_bits];

  std::vector<const char *> names;

  bump_allocator name_alloc;
  bump_allocator tree_item_alloc;
  split2monodb &db;
  mmapped_file &svn2git;
  sha1_pool &pool;
  dir_list &dirs;
};
} // end namespace

int dir_list::add_dir(const char *name, bool &is_new, int &d) {
  if (!name || !*name)
    return 1;
  dir_type dir(name);
  for (const char *ch = name; *ch; ++ch) {
    if (*ch >= 'a' && *ch <= 'z')
      continue;
    if (*ch >= 'Z' && *ch <= 'Z')
      continue;
    if (*ch >= '0' && *ch <= '9')
      continue;
    switch (*ch) {
    default:
      return 1;
    case '_':
    case '-':
    case '+':
    case '.':
      continue;
    }
  }

  bool found = false;
  d = lookup_dir(name, found);
  is_new = !found;
  if (is_new)
    list.insert(list.begin() + d, dir);
  return 0;
}
int dir_list::lookup_dir(const char *name, bool &found) {
  found = false;
  return bisect_first_match(list.begin(), list.end(),
                            [&name, &found](const dir_type &dir) {
                              int diff = strcmp(name, dir.name);
                              found |= !diff;
                              return diff <= 0;
                            }) -
         list.begin();
}

void git_cache::note_commit_tree(sha1_ref commit, sha1_ref tree) {
  assert(tree);
  auto &entry = commit_trees[commit->get_bits(0, num_cache_bits)];
  entry.key = commit;
  entry.value = tree;
}

void git_cache::note_rev(sha1_ref commit, int rev) {
  auto &entry = revs[commit->get_bits(0, num_cache_bits)];
  entry.commit = commit;
  entry.rev = rev;
}

void git_cache::note_mono(sha1_ref split, sha1_ref mono) {
  assert(mono);
  auto &entry = monos[split->get_bits(0, num_cache_bits)];
  entry.key = split;
  entry.value = mono;
}

void git_cache::note_tree(const git_tree &tree) {
  trees[tree.sha1->get_bits(0, num_cache_bits)] = tree;
}

int git_cache::lookup_commit_tree(sha1_ref commit, sha1_ref &tree) const {
  auto &entry = commit_trees[commit->get_bits(0, num_cache_bits)];
  if (entry.key != commit)
    return 1;
  tree = entry.value;
  return 0;
}

int git_cache::lookup_rev(sha1_ref commit, int &rev) const {
  auto &entry = revs[commit->get_bits(0, num_cache_bits)];
  if (entry.commit != commit)
    return 1;
  rev = entry.rev;
  return 0;
}

int git_cache::lookup_mono(sha1_ref split, sha1_ref &mono) const {
  auto &entry = monos[split->get_bits(0, num_cache_bits)];
  if (entry.key != split)
    return 1;
  mono = entry.value;
  return 0;
}

int git_cache::lookup_tree(git_tree &tree) const {
  auto &entry = trees[tree.sha1->get_bits(0, num_cache_bits)];
  if (entry.sha1 != tree.sha1)
    return 1;
  tree = entry;
  return 0;
}

int git_cache::set_mono(sha1_ref split, sha1_ref mono) {
  if (commits_query(*split).insert_data(db.commits, *mono))
    return 1;
  note_mono(split, mono);
  return 0;
}
int git_cache::get_mono(sha1_ref split, sha1_ref &mono) {
  if (!lookup_mono(split, mono))
    return 0;

  binary_sha1 sha1;
  if (!commits_query(*split).lookup_data(db.commits, sha1)) {
    mono = pool.lookup(sha1);
    note_mono(split, mono);
    return 0;
  }

  int rev = -1;
  if (get_rev(split, rev) || rev <= 0)
    return 1;

  auto *bytes = reinterpret_cast<const unsigned char *>(svn2git.bytes);
  long offset = 20 * rev;
  if (offset + 20 > svn2git.num_bytes)
    return 1;
  sha1.from_binary(bytes + offset);
  mono = pool.lookup(sha1);
  if (!mono)
    return 1;
  note_mono(split, mono);
  return 0;
}

int git_cache::get_commit_tree(sha1_ref commit, sha1_ref &tree) {
  if (!lookup_commit_tree(commit, tree))
    return 0;

  bool once = false;
  auto reader = [&](std::string line) {
    if (once)
      return 1;
    once = true;

    textual_sha1 text;
    if (text.from_input(line.c_str()))
      return 1;

    tree = pool.lookup(text);
    note_commit_tree(commit, tree);
    return 0;
  };

  assert(commit);
  std::string ref = textual_sha1(*commit).bytes;
  ref += "^{tree}";
  const char *argv[] = {"git", "rev-parse", "--verify", ref.c_str(), nullptr};
  return call_git(argv, nullptr, reader);
}

int git_cache::get_rev(sha1_ref commit, int &rev) {
  if (!lookup_rev(commit, rev))
    return 0;

  {
    svnbaserev dbrev;
    if (!svnbase_query(*commit).lookup_data(db.svnbase, dbrev)) {
      // Negative indicates it's not upstream.
      rev = -dbrev.get_rev();
      note_rev(commit, rev);
      return 0;
    }
  }

  const char *llvm_rev_trailer = "llvm-rev: ";
  const int llvm_rev_trailer_len = strlen(llvm_rev_trailer);
  const char *git_svn_id_trailer =
      "git-svn-id: https://llvm.org/svn/llvm-project/";
  const int git_svn_id_trailer_len = strlen(git_svn_id_trailer);

  bool found = false;
  long parsed_rev = -1;
  std::string timestamp;
  int count = 0;
  auto reader = [&](std::string line) {
    switch (count++) {
    default:
      break;
    case 0:
      timestamp = std::move(line);
      return 0;
    case 1:
      if (line == timestamp)
        return 0;
      // Author and commit timestamps don't match.  Looks like a cherry-pick.
      return EOF;
    }
    if (found)
      return 1;

    // Check for "llvm-rev: <rev>".
    if (!line.compare(0, llvm_rev_trailer_len, llvm_rev_trailer)) {
      char *end_rev = nullptr;
      parsed_rev = strtol(line.data() + llvm_rev_trailer_len, &end_rev, 10);
      if (*end_rev)
        return 0;
      found = true;
      return EOF;
    }

    // Check for "git-svn-id: <url>@<rev> <junk>".
    if (line.compare(0, git_svn_id_trailer_len, git_svn_id_trailer))
      return 0;
    size_t at = line.find('@', git_svn_id_trailer_len);
    if (at == std::string::npos)
      return 0;

    char *end_rev = nullptr;
    parsed_rev = strtol(line.data() + at + 1, &end_rev, 10);
    if (*end_rev != ' ')
      return 0;
    found = true;
    return EOF;
  };

  textual_sha1 sha1(*commit);
  const char *argv[] = {"git", "log",      "--format=format:%at%n%ct%n%B",
                        "-1",  sha1.bytes, nullptr};
  if (call_git(argv, nullptr, reader))
    return error("failed to look up svnbaserev in git for " +
                 commit->to_string());

  if (!found) {
    // FIXME: consider warning here.
    rev = 0;
    note_rev(commit, rev);
    return 0;
  }

  if (parsed_rev > INT_MAX)
    return error("missing llvm-svn-base-rev for " + commit->to_string() +
                 " is too big");
  rev = parsed_rev;
  note_rev(commit, rev);
  return 0;
}

const char *git_cache::make_name(const char *name, size_t len) {
  bool found = false;
  auto d = bisect_first_match(dirs.list.begin(), dirs.list.end(),
                              [&name, &found](const dir_type &dir) {
                                int diff = strcmp(name, dir.name);
                                found |= !diff;
                                return diff <= 0;
                              });
  assert(!found || d != dirs.list.end());
  if (found)
    return d->name;

  auto n = bisect_first_match(names.begin(), names.end(),
                              [&name, &found](const char *x) {
                                int diff = strcmp(name, x);
                                found |= !diff;
                                return diff <= 0;
                              });
  assert(!found || n != names.end());
  if (found)
    return *n;
  char *allocated = new (name_alloc) char[len + 1];
  strncpy(allocated, name, len);
  allocated[len] = 0;
  return *names.insert(n, allocated);
}

git_tree::item_type *git_cache::make_items(git_tree::item_type *first,
                                           git_tree::item_type *last) {
  if (first == last)
    return nullptr;
  auto *items = new (tree_item_alloc) git_tree::item_type[last - first];
  std::move(first, last, items);
  return items;
}

int git_cache::ls_tree(git_tree &tree) {
  if (!lookup_tree(tree))
    return 0;

  constexpr const int max_items = dir_mask::max_size;
  git_tree::item_type items[max_items];
  git_tree::item_type *last = items;
  auto reader = [&](std::string line) {
    if (last - items == max_items)
      return 1;

    size_t space1 = line.find(' ');
    size_t space2 = line.find(' ', space1 + 1);
    size_t tab = line.find('\t', space2 + 1);
    if (!line.compare(0, space1, "100755"))
      last->is_exec = true;
    else if (line.compare(0, space1, "100644"))
      return 1;

    if (!line.compare(space1 + 1, space2 - space1 - 1, "tree"))
      last->is_tree = true;
    else if (line.compare(space1 + 1, space2 - space1 - 1, "blob"))
      return 1;

    last->name = make_name(line.c_str() + tab + 1, line.size() - tab - 1);
    if (!*last->name)
      return 1;

    textual_sha1 text;
    line[tab] = '\0';
    if (text.from_input(&line[space2 + 1]))
      return 1;
    last->sha1 = pool.lookup(text);
    ++last;
    return 0;
  };

  std::string ref = tree.sha1->to_string();
  const char *args[] = {"git", "ls-tree", ref.c_str(), nullptr};
  if (call_git(args, nullptr, reader))
    return 1;

  tree.num_items = last - items;
  tree.items = make_items(items, last);
  note_tree(tree);
  return 0;
}

int git_cache::mktree(git_tree &tree) {
  assert(!tree.sha1);
  bool once = false;
  auto reader = [&](std::string line) {
    if (once)
      return 1;
    once = true;

    textual_sha1 text;
    if (text.from_input(line.c_str()))
      return 1;

    tree.sha1 = pool.lookup(text);
    note_tree(tree);
    return 0;
  };

  auto writer = [&](FILE *file, bool &stop) {
    assert(!stop);
    for (auto i = 0; i != tree.num_items; ++i) {
      assert(tree.items[i].sha1);
      if (!fprintf(file, "%s %s %s\t%s\n",
                   tree.items[i].is_exec ? "100755" : "100644",
                   tree.items[i].is_tree ? "tree" : "blob",
                   textual_sha1(*tree.items[i].sha1).bytes, tree.items[i].name))
        return 1;
    }
    stop = true;
    return 0;
  };

  const char *argv[] = {"git", "mktree", nullptr};
  return call_git(argv, nullptr, reader, writer);
}

static int get_commit_metadata(sha1_ref commit,
                               git_cache::commit_tree_buffers &buffers) {
  auto &message = buffers.message;
  const char *prefixes[] = {
      "GIT_AUTHOR_NAME=",    "GIT_COMMITTER_NAME=", "GIT_AUTHOR_DATE=",
      "GIT_COMMITTER_DATE=", "GIT_AUTHOR_EMAIL=",   "GIT_COMMITTER_EMAIL="};
  std::string *vars[] = {&buffers.an, &buffers.cn, &buffers.ad,
                         &buffers.cd, &buffers.ae, &buffers.ce};
  for (int i = 0; i < 6; ++i)
    *vars[i] = prefixes[i];

  message.clear();
  textual_sha1 sha1(*commit);
  const char *args[] = {"git",
                        "log",
                        "--date=raw",
                        "-1",
                        "--format=format:%an%n%cn%n%ad%n%cd%n%ae%n%ce%n%B",
                        sha1.bytes,
                        nullptr};
  size_t count = 0;
  auto reader = [&message, &count, &vars](std::string line) {
    if (count++ < 6) {
      vars[count - 1]->append(line);
      return 0;
    }

    message.append(line);
    message += '\n';
    return 0;
  };
  if (call_git(args, nullptr, reader))
    return error(std::string("failed to read commit message for ") +
                 sha1.bytes);
  if (count < 6)
    return error(std::string("missing commit metadata for ") + sha1.bytes);
  return 0;
}

static bool should_separate_trailers(const std::string &message) {
  if (message.size() < 2)
    return false;
  assert(message.end()[-1] == '\n');
  if (message.end()[-2] == '\n')
    return false;
  size_t start = message.rfind('\n', message.size() - 2);
  start = start == std::string::npos ? 0 : start + 1;
  const char *ch = message.c_str() + start;
  for (; *ch; ++ch) {
    if (*ch >= 'a' && *ch <= 'z')
      continue;
    if (*ch >= 'Z' && *ch <= 'Z')
      continue;
    if (*ch >= '0' && *ch <= '9')
      continue;
    if (*ch == '_' || *ch == '-' || *ch == '+')
      continue;
    if (*ch == ':')
      return *++ch != ' ';
    return true;
  }
  return true;
}

static void append_trailers(const char *dir, sha1_ref base_commit,
                            std::string &message) {
  if (should_separate_trailers(message))
    message += '\n';
  textual_sha1 sha1(*base_commit);
  message += "apple-llvm-split-commit: ";
  message += sha1.bytes;
  message += '\n';
  message += "apple-llvm-split-dir: ";
  message += dir;
  if (dir[0] != '-' || dir[1])
    message += '/';
  message += '\n';
}

int git_cache::commit_tree(sha1_ref base_commit, const char *dir, sha1_ref tree,
                           const std::vector<sha1_ref> &parents,
                           sha1_ref &commit, commit_tree_buffers &buffers) {
  if (get_commit_metadata(base_commit, buffers))
    return error("failed to get metadata for " + base_commit->to_string());
  append_trailers(dir, base_commit, buffers.message);

  const char *envp[] = {buffers.an.c_str(),
                        buffers.ae.c_str(),
                        buffers.ad.c_str(),
                        buffers.cn.c_str(),
                        buffers.ce.c_str(),
                        buffers.cd.c_str(),
                        nullptr};

  buffers.parents.clear();
  for (sha1_ref p : parents)
    buffers.parents.emplace_back(*p);

  textual_sha1 text_tree(*tree);
  buffers.args.clear();
  buffers.args.push_back("git");
  buffers.args.push_back("commit-tree");
  buffers.args.push_back(text_tree.bytes);
  for (auto &p : buffers.parents) {
    buffers.args.push_back("-p");
    buffers.args.push_back(p.bytes);
  }
  buffers.args.push_back(nullptr);

  bool found = false;
  auto reader = [&](std::string line) {
    if (found)
      return error("extra lines in new commit");
    found = true;
    textual_sha1 sha1;
    const char *end = nullptr;
    if (sha1.from_input(line.c_str(), &end) ||
        end != line.c_str() + line.size())
      return error("invalid sha1 for new commit");
    commit = pool.lookup(sha1);
    note_commit_tree(commit, tree);
    return 0;
  };
  auto writer = [&buffers](FILE *file, bool &stop) {
    fprintf(file, "%s", buffers.message.c_str());
    stop = true;
    return 0;
  };
  if (call_git(buffers.args.data(), envp, reader, writer))
    return 1;

  if (!found)
    return error("missing sha1 for new commit");
  return 0;
}
