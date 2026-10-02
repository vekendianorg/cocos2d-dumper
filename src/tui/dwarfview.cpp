// SPDX-License-Identifier: MIT
#include "stellar/tui/dwarfview.h"

#include <algorithm>
#include <chrono>
#include <exception>

#include "stellar/dwarf/constants.h"
#include "stellar/dwarf/dwarf_context.h"
#include "stellar/elf/elf_file.h"

namespace stellar::tui {

namespace {
using Clock = std::chrono::steady_clock;
double since(Clock::time_point t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}
}  // namespace

UnitList list_units(const std::string& path, std::uint64_t max_rows) {
  UnitList out;
  try {
    elf::ElfFile elf;
    std::string err;
    if (!elf.open(path, &err)) {
      out.error = err;
      return out;
    }
    dwarf::DwarfContext ctx(elf);
    if (!ctx.sections().has_info()) {
      out.error = "no .debug_info: nothing to list";
      return out;
    }
    out.total = ctx.unit_count();
    dwarf::DwarfContext::UnitIterator it(ctx);
    dwarf::UnitHeader h;
    std::string uerr;
    while (it.next(h, &uerr)) {
      if (out.rows.size() >= max_rows) {
        out.truncated = true;
        break;
      }
      UnitRow r;
      r.index = it.index();
      r.offset = h.offset;
      r.abbrev_offset = h.abbrev_offset;
      r.version = h.version;
      r.address_size = h.address_size;
      out.rows.push_back(r);
    }
    out.unparsable = it.skipped_errors();
    out.ok = true;
  } catch (const std::exception& e) {
    out.ok = false;
    out.error = e.what();
  }
  return out;
}

bool count_unit_dies(const std::string& path, std::uint64_t index, std::uint64_t& dies,
                     std::string* error) {
  try {
    elf::ElfFile elf;
    std::string err;
    if (!elf.open(path, &err)) {
      if (error) *error = err;
      return false;
    }
    dwarf::DwarfContext ctx(elf);
    dwarf::DwarfContext::UnitIterator it(ctx);
    dwarf::UnitHeader h;
    while (it.next(h, &err)) {
      if (it.index() != index) continue;
      const dwarf::AbbrevTable* ab = ctx.abbrev_table(h, &err);
      if (ab == nullptr) {
        if (error) *error = err.empty() ? "abbreviation table unreadable" : err;
        return false;
      }
      dwarf::UnitWalker w(ctx.info(), h, ab);
      w.reset();
      dwarf::Die d;
      dies = 0;
      while (w.next(d)) ++dies;
      return true;
    }
    if (error) *error = "unit not found";
  } catch (const std::exception& e) {
    if (error) *error = e.what();
  }
  return false;
}

ScanJob::~ScanJob() noexcept {
  cancel_.store(true);
  join();
}

void ScanJob::join() noexcept {
  if (worker_.joinable()) worker_.join();
}

bool ScanJob::start(const std::string& path) {
  if (running_.load()) return false;
  join();
  cancel_.store(false);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snap_ = ScanSnapshot{};
    snap_.phase = ScanSnapshot::Phase::kRunning;
  }
  running_.store(true);
  try {
    worker_ = std::thread([this, path] { run(path); });
  } catch (const std::exception& e) {
    running_.store(false);
    std::lock_guard<std::mutex> lock(mutex_);
    snap_.phase = ScanSnapshot::Phase::kFailed;
    snap_.error = std::string("could not start the scan thread: ") + e.what();
    return false;
  }
  return true;
}

ScanSnapshot ScanJob::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snap_;
}

void ScanJob::run(std::string path) {
  const auto t0 = Clock::now();
  const auto finish = [&](ScanSnapshot::Phase p, std::string err) {
    std::lock_guard<std::mutex> lock(mutex_);
    snap_.phase = p;
    snap_.error = std::move(err);
    snap_.elapsed_seconds = since(t0);
    running_.store(false);
  };
  try {
    elf::ElfFile elf;
    std::string err;
    if (!elf.open(path, &err)) {
      finish(ScanSnapshot::Phase::kFailed, err);
      return;
    }
    dwarf::DwarfContext ctx(elf);
    if (!ctx.sections().has_info()) {
      finish(ScanSnapshot::Phase::kFailed, "no .debug_info: nothing to scan");
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      snap_.units_total = ctx.unit_count();
    }
    dwarf::WalkStats st;
    dwarf::DwarfContext::UnitIterator it(ctx);
    dwarf::UnitHeader h;
    std::uint64_t since_publish = 0;
    const auto publish = [&] {
      std::lock_guard<std::mutex> lock(mutex_);
      snap_.units = st.units;
      snap_.dies = st.dies;
      snap_.max_depth = st.max_depth;
      snap_.bytes = st.bytes_scanned;
      snap_.failed_units = st.failed_units;
      snap_.skipped_units = it.skipped_errors();
      snap_.elapsed_seconds = since(t0);
    };
    while (it.next(h, &err)) {
      if (cancel_.load()) {
        publish();
        finish(ScanSnapshot::Phase::kCancelled, {});
        return;
      }
      const dwarf::AbbrevTable* ab = ctx.abbrev_table(h, &err);
      if (ab == nullptr) {
        ++st.failed_units;
        continue;
      }
      dwarf::UnitWalker w(ctx.info(), h, ab);
      w.reset();
      dwarf::Die d;
      while (w.next(d)) {
        ++st.dies;
        if (d.tag() < st.tag_counts.size()) ++st.tag_counts[d.tag()];
        if (d.depth() > st.max_depth) st.max_depth = d.depth();
      }
      st.bytes_scanned += h.body_size();
      ++st.units;
      if (++since_publish >= 16) {  // cheap enough to be live, rare enough to be free
        since_publish = 0;
        publish();
      }
    }
    publish();
    std::vector<std::pair<std::string, std::uint64_t>> tags;
    for (std::size_t t = 0; t < st.tag_counts.size(); ++t) {
      if (st.tag_counts[t] == 0) continue;
      tags.emplace_back(std::string(dwarf::tag_name(static_cast<std::uint32_t>(t))),
                        st.tag_counts[t]);
    }
    std::stable_sort(tags.begin(), tags.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    {
      std::lock_guard<std::mutex> lock(mutex_);
      snap_.tags = std::move(tags);
    }
    finish(ScanSnapshot::Phase::kDone, {});
  } catch (const std::exception& e) {
    finish(ScanSnapshot::Phase::kFailed, e.what());
  }
}

}  // namespace stellar::tui
