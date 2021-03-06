/*!
 *  Copyright (c) 2017 by Contributors
 * \file coproc_sync.cc
 */
#include <tvm/ir.h>
#include <tvm/ir_pass.h>
#include <tvm/ir_mutator.h>
#include <tvm/ir_visitor.h>
#include <unordered_map>
#include <unordered_set>
#include "./ir_util.h"
#include "./storage_access.h"

namespace tvm {
namespace ir {

// Visitor to find touched set by co-processor scope.
class CoProcTouchedBuffer : public IRVisitor {
 public:
  void Visit_(const Load* op) final {
    if (in_scope_) {
      touched_[op->buffer_var.get()].coproc = true;
    } else {
      touched_[op->buffer_var.get()].normal = true;
    }
    IRVisitor::Visit_(op);
  }
  void Visit_(const Store* op) final {
    if (in_scope_) {
      touched_[op->buffer_var.get()].coproc = true;
    } else {
      touched_[op->buffer_var.get()].normal = true;
    }
    IRVisitor::Visit_(op);
  }
  void Visit_(const Call* op) final {
    if (op->is_intrinsic(intrinsic::tvm_access_ptr)) {
      const Variable* buffer = op->args[1].as<Variable>();
      if (in_scope_) {
        touched_[buffer].coproc = true;
      } else {
        touched_[buffer].normal = true;
      }
    }
    IRVisitor::Visit_(op);
  }
  void Visit_(const AttrStmt* op) final {
    if (op->attr_key == attr::coproc_scope && !in_scope_) {
      in_scope_ = true;
      IterVar iv(op->node.node_);
      coproc_.insert(iv);
      IRVisitor::Visit_(op);
      in_scope_ = false;
    } else {
      IRVisitor::Visit_(op);
    }
  }

  // Touch Entry
  struct TouchEntry {
    bool normal{false};
    bool coproc{false};
  };
  std::unordered_map<const Variable*, TouchEntry> touched_;
  std::unordered_set<IterVar> coproc_;

 private:
  bool in_scope_{false};
};

// Synchronization planning with co-processor.
class CoProcSyncPlanner : public StorageAccessVisitor {
 public:
  explicit CoProcSyncPlanner(
      const std::unordered_set<const Variable*>& touched,
      const std::string& coproc_name)
      : touched_(touched), coproc_name_(coproc_name) {
  }

  void Plan(const Stmt& stmt) {
    this->Visit(stmt);
    PlanSync(scope_.back(), nullptr, true);
    if (sync_.size() == 0) {
      sync_[stmt.get()] = GetSync(coproc_name_ + ".coproc_sync");
    }
  }

  // Write synchronization to be inserted before or after stmt.
  std::unordered_map<const Node*, std::vector<Stmt> > sync_;

 protected:
  bool Enabled(const Variable* buf,
               const StorageScope& scope) const final {
    return touched_.count(buf);
  }

  // Plan the sync
  std::vector<AccessEntry> Summarize(
      std::vector<StmtEntry> seq, const For* loop) final {
    return PlanSync(seq, loop, false);
  }

 private:
  // Plan write synchronization if write is not coherent
  std::vector<AccessEntry> PlanSync(
      std::vector<StmtEntry> seq, const For* loop,
      bool force_sync_at_end) {
    // detect write barriers
    // access by the co-processor.
    std::vector<AccessEntry> co_access;
    bool contain_sync = false;

    auto find_conflict = [&](const AccessEntry& acc) {
      for (const AccessEntry& x : co_access) {
        if (x.buffer.same_as(acc.buffer) &&
            ((acc.type == kRead && x.type == kWrite) ||
             acc.type == kWrite)) {
          return true;
        }
      }
      return false;
    };
    for (size_t i = 0; i < seq.size(); ++i) {
      const StmtEntry& s = seq[i];
      bool sync_write = false;
      for (const AccessEntry& acc : s.access) {
        if (acc.threads.size() == 0 && find_conflict(acc)) {
          sync_write = true; break;
        }
        if (acc.type == kSync) {
          co_access.clear();
          contain_sync = true;
        }
      }
      if (sync_write) {
        CHECK_NE(i, 0U);
        sync_[seq[i - 1].stmt] = GetSync(co_access);
        co_access.clear();
        contain_sync = true;
      }
      for (const AccessEntry& acc : s.access) {
        if (acc.threads.size() != 0) {
          co_access.push_back(acc);
        }
      }
    }
    bool sync_at_end = force_sync_at_end;
    if (loop != nullptr && !sync_at_end) {
      // loop carray dependency
      for (size_t i = 0; i < seq.size(); ++i) {
        const StmtEntry& s = seq[i];
        for (const AccessEntry& acc : s.access) {
          if (acc.threads.size() == 0 && find_conflict(acc)) {
            sync_at_end = true; break;
          }
        }
        if (sync_.count(s.stmt) || sync_at_end) break;
      }
    }
    if (sync_at_end && co_access.size() != 0) {
      CHECK_NE(seq.size(), 0);
      contain_sync = true;
      sync_[seq.back().stmt] = GetSync(co_access);
      co_access.clear();
    }
    if (contain_sync) {
      AccessEntry e;
      e.type = kSync;
      co_access.insert(co_access.begin(), e);
    }
    return co_access;
  }
  // Add write Synchronization
  std::vector<Stmt> GetSync(const std::vector<AccessEntry>& co_access) {
    // Does not consider memory coherence, need runtime.
    CHECK_NE(co_access.size(), 0U);
    CHECK_EQ(co_access[0].threads.size(), 1U);
    return GetSync(coproc_name_ + ".coproc_sync");
  }

  std::vector<Stmt> GetSync(std::string sync_name) {
    return {Evaluate::make(Call::make(
        Int(32),
        sync_name,
        {}, Call::Intrinsic))};
  }

  const std::unordered_set<const Variable*>& touched_;
  std::string coproc_name_;
};

// Detect memory barriers when coproc read/write memory
class CoProcBarrierDetector : public StorageAccessVisitor {
 public:
  explicit CoProcBarrierDetector(
      const std::unordered_set<const Variable*>& touched,
      const std::string& coproc_name)
      : touched_(touched) {
    read_barrier_name_ = coproc_name + ".coproc_read_barrier";
    write_barrier_name_ = coproc_name + ".coproc_write_barrier";
  }

  void PlanReadBarrier(Stmt stmt) {
    read_barrier_ = true;
    this->Visit(stmt);
    PlanReadBarrier(scope_.back(), nullptr);
  }
  void PlanWriteBarrier(Stmt stmt) {
    read_barrier_ = false;
    this->Visit(stmt);
    PlanWriteBarrier(scope_.back(), nullptr);
  }

  std::unordered_map<const Node*, std::vector<Stmt> > barrier_before_;
  std::unordered_map<const Node*, std::vector<Stmt> > barrier_after_;

 protected:
  bool Enabled(const Variable* buf,
               const StorageScope& scope) const final {
    return touched_.count(buf);
  }

  // Plan the sync
  std::vector<AccessEntry> Summarize(
      std::vector<StmtEntry> seq, const For* loop) final {
    if (read_barrier_) {
      return PlanReadBarrier(seq, loop);
    } else {
      return PlanWriteBarrier(seq, loop);
    }
  }

 private:
  // Plan write barrier at Read after write point.
  std::vector<AccessEntry> PlanWriteBarrier(
      std::vector<StmtEntry> seq, const For* loop) {
    std::vector<AccessEntry> read_seq;
    std::unordered_map<const Variable*, std::vector<AccessEntry> > write_set;

    auto fupdate = [&](size_t i, const AccessEntry& acc) {
      auto it  = write_set.find(acc.buffer.get());
      if (it != write_set.end()) {
        CHECK_NE(i, 0U);
        barrier_after_[seq[i - 1].stmt].push_back(
            MakeBarrier(write_barrier_name_, it->second));
        write_set.erase(it);
      }
    };
    for (size_t i = 0; i < seq.size(); ++i) {
      const StmtEntry& s = seq[i];
      for (const AccessEntry& acc : s.access) {
        if (acc.threads.size() == 0 && acc.type == kRead) {
          fupdate(i, acc);
          read_seq.push_back(acc);
        }
      }
      for (const AccessEntry& acc : s.access) {
        if (acc.threads.size() != 0 && acc.type == kWrite) {
          write_set[acc.buffer.get()].push_back(acc);
        }
      }
    }
    // loop carry
    if (loop != nullptr) {
      for (const AccessEntry& acc : read_seq) {
        fupdate(seq.size(), acc);
      }
    }
    for (const auto &kv : write_set) {
      read_seq.insert(read_seq.end(), kv.second.begin(), kv.second.end());
    }
    return read_seq;
  }

  std::vector<AccessEntry> PlanReadBarrier(
      std::vector<StmtEntry> seq, const For* loop) {
    std::vector<AccessEntry> write_seq;
    std::unordered_map<const Variable*, std::vector<AccessEntry> > read_set;

    auto fupdate = [&](size_t i, const AccessEntry& acc) {
      auto it  = read_set.find(acc.buffer.get());
      if (it != read_set.end()) {
        CHECK_NE(i, seq.size());
        barrier_before_[seq[i].stmt].push_back(
            MakeBarrier(read_barrier_name_, it->second));
        read_set.erase(it);
      }
    };

    for (size_t i = seq.size(); i != 0; --i) {
      const StmtEntry& s = seq[i - 1];
      for (const AccessEntry& acc : s.access) {
        if (acc.threads.size() == 0 && acc.type == kWrite) {
          fupdate(i, acc);
          write_seq.push_back(acc);
        }
      }
      for (const AccessEntry& acc : s.access) {
        if (acc.threads.size() != 0 && acc.type == kRead) {
          read_set[acc.buffer.get()].push_back(acc);
        }
      }
    }
    // loop carry
    if (loop != nullptr) {
      for (const AccessEntry& acc : write_seq) {
        fupdate(0, acc);
      }
    }
    for (const auto &kv : read_set) {
      write_seq.insert(write_seq.end(), kv.second.begin(), kv.second.end());
    }
    return write_seq;
  }

  Stmt MakeBarrier(const std::string& func, const std::vector<AccessEntry>& wvec) {
    // insert write point
    Array<arith::IntSet> wset;
    for (const AccessEntry& acc : wvec) {
      CHECK(acc.dtype == wvec[0].dtype);
      wset.push_back(acc.touched);
    }
    Range none;
    Range r = arith::Union(wset).cover_range(none);
    CHECK(r.defined())
        << "Cannot deduce write range of " << wvec[0].buffer;
    Expr min = r->min;
    Expr extent = r->extent;
    return Evaluate::make(Call::make(
        Int(32), func,
        {wvec[0].buffer, wvec[0].dtype.bits(), r->min, r->extent}, Call::Intrinsic));
  }
  // Write barrier name
  bool read_barrier_{false};
  std::string read_barrier_name_;
  std::string write_barrier_name_;
  const std::unordered_set<const Variable*>& touched_;
};


class CoProcSyncInserter : public IRMutator {
 public:
  Stmt Insert(Stmt stmt) {
    CoProcTouchedBuffer visitor;
    visitor.Visit(stmt);
    if (visitor.coproc_.size() == 0) return stmt;
    std::unordered_set<const Variable*> touched;

    for (const auto &kv : visitor.touched_) {
      if (kv.second.normal && kv.second.coproc) {
        touched.insert(kv.first);
      }
    }
    CHECK_EQ(visitor.coproc_.size(), 1U);
    std::string coproc_name = (*visitor.coproc_.begin())->var->name_hint;
    // plan sync.
    CoProcSyncPlanner sync_planner(touched, coproc_name);
    sync_planner.Plan(stmt);
    for (const auto& kv : sync_planner.sync_) {
      auto& vec = insert_after_[kv.first];
      vec.insert(vec.end(), kv.second.begin(), kv.second.end());
    }
    // Detect barrier
    CoProcBarrierDetector barrier_detector(touched, coproc_name);
    barrier_detector.PlanReadBarrier(stmt);
    barrier_detector.PlanWriteBarrier(stmt);
    for (const auto& kv : barrier_detector.barrier_before_) {
      auto& vec = insert_before_[kv.first];
      vec.insert(vec.end(), kv.second.begin(), kv.second.end());
    }
    for (const auto& kv : barrier_detector.barrier_after_) {
      auto& vec = insert_after_[kv.first];
      vec.insert(vec.end(), kv.second.begin(), kv.second.end());
    }
    return Mutate(stmt);
  }

  Stmt Mutate(Stmt stmt) final {
    Stmt before, after;
    auto it = insert_before_.find(stmt.get());
    if (it != insert_before_.end()) {
      before = MergeSeq(it->second);
    }
    it = insert_after_.find(stmt.get());
    if (it != insert_after_.end()) {
      after = MergeSeq(it->second);
    }
    stmt = IRMutator::Mutate(stmt);
    if (before.defined()) {
      stmt = Block::make(before, stmt);
    }
    if (after.defined()) {
      stmt = Block::make(stmt, after);
    }
    return stmt;
  }

 private:
  std::unordered_map<const Node*, std::vector<Stmt> > insert_before_;
  std::unordered_map<const Node*, std::vector<Stmt> > insert_after_;
};

Stmt CoProcSync(Stmt stmt) {
  return CoProcSyncInserter().Insert(stmt);
}

}  // namespace ir
}  // namespace tvm
