use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;

/// A single search result.
#[derive(Clone, Debug)]
pub struct SearchHit {
    pub path: PathBuf,
    pub name: String,
    pub is_dir: bool,
    pub size: u64,
}

/// A group of duplicate files (same size and same content hash).
#[derive(Clone, Debug)]
pub struct DupGroup {
    pub size: u64,
    pub hash: String,
    pub files: Vec<PathBuf>,
}

/// One computed folder: recursive size and contained entry count.
#[derive(Clone, Debug)]
pub struct DirSize {
    pub path: String,
    pub bytes: u64,
    pub items: u64,
}

/// Kind of one side-by-side comparison row.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum DiffKind {
    /// Line present in both files (identical).
    Same,
    /// Line only in the left file (removed relative to the right).
    Removed,
    /// Line only in the right file (added relative to the left).
    Added,
    /// Both sides present but different (replaced pair).
    Replaced,
    /// Skipped block of unchanged lines; payload is the skipped count.
    Gap,
}

/// One aligned row of the side-by-side comparison view: `None` on a side
/// means that file has no line at this position.
#[derive(Clone, Debug)]
pub struct DiffRow {
    pub kind: DiffKind,
    pub lno: Option<u32>,
    pub left: Option<String>,
    pub rno: Option<u32>,
    pub right: Option<String>,
}

/// Result payload delivered by a finished background job.
#[derive(Clone, Debug)]
pub enum Outcome {
    Hash {
        path: String,
        md5: Option<String>,
        sha256: Option<String>,
    },
    Search {
        hits: Vec<SearchHit>,
        scanned: u64,
        truncated: bool,
    },
    Dup {
        groups: Vec<DupGroup>,
        scanned: u64,
        truncated: bool,
    },
    Sizes {
        sizes: Vec<DirSize>,
    },
    Compare {
        rows: Vec<DiffRow>,
        added: usize,
        removed: usize,
        same: usize,
        truncated: bool,
    },
}

/// A transfer hit an existing target and asked the user what to do.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ConflictInfo {
    pub path: String,
    pub is_dir: bool,
}

/// Decision for a conflicting target.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Decision {
    Overwrite,
    Skip,
}

/// Live state of a background job, shared with the worker thread.
#[derive(Clone, Default, Debug)]
pub struct JobState {
    pub current: String,
    pub done: u64,
    pub total: u64,
    pub finished: bool,
    pub error: Option<String>,
    pub outcome: Option<Outcome>,
    /// Set while a transfer waits for an overwrite decision.
    pub conflict: Option<ConflictInfo>,
    /// Answer for the pending conflict (written by the UI thread).
    pub decision: Option<Decision>,
    /// Answer applied automatically to all future conflicts of this job.
    pub auto_decision: Option<Decision>,
}

/// Block the worker until the UI answers the pending conflict (or the job
/// is cancelled). Returns the decision, or `None` when cancelled.
pub fn await_decision(
    state: &Arc<Mutex<JobState>>,
    cancel: &Arc<AtomicBool>,
) -> Option<Decision> {
    loop {
        if cancel.load(Ordering::Relaxed) {
            return None;
        }
        if let Ok(mut s) = state.lock() {
            if let Some(d) = s.auto_decision {
                s.conflict = None;
                return Some(d);
            }
            if let Some(d) = s.decision.take() {
                s.conflict = None;
                return Some(d);
            }
        }
        std::thread::sleep(std::time::Duration::from_millis(50));
    }
}

/// What kind of work a job performs (drives result routing and panel reloads).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum JobKind {
    Copy,
    Move,
    Delete,
    Hash,
    Search,
    Dup,
    Sizes,
    Compare,
}

/// A background job running on a worker thread with progress and cancellation.
pub struct Job {
    pub id: u64,
    pub kind: JobKind,
    pub label: String,
    state: Arc<Mutex<JobState>>,
    cancel: Arc<AtomicBool>,
    #[allow(dead_code)]
    handle: Option<JoinHandle<()>>,
}

/// Sequence number for `Job::id`.
static NEXT_JOB_ID: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);

impl Job {
    /// Spawn a job. `f` receives the shared state and a cancellation flag and
    /// is expected to mark the state as `finished` before returning.
    pub fn spawn<F>(kind: JobKind, label: String, f: F) -> Self
    where
        F: FnOnce(&Arc<Mutex<JobState>>, &Arc<AtomicBool>) + Send + 'static,
    {
        let state = Arc::new(Mutex::new(JobState::default()));
        let cancel = Arc::new(AtomicBool::new(false));
        let st = state.clone();
        let cx = cancel.clone();
        let handle = std::thread::spawn(move || f(&st, &cx));
        Self {
            id: NEXT_JOB_ID.fetch_add(1, Ordering::Relaxed),
            kind,
            label,
            state,
            cancel,
            handle: Some(handle),
        }
    }

    pub fn snapshot(&self) -> JobState {
        self.state.lock().map(|s| s.clone()).unwrap_or_default()
    }

    pub fn cancel(&self) {
        self.cancel.store(true, Ordering::SeqCst);
    }

    /// Answer a pending overwrite conflict and optionally fix the answer for
    /// all remaining conflicts of this job.
    pub fn resolve(&self, decision: Decision, all: bool) {
        if let Ok(mut s) = self.state.lock() {
            s.decision = Some(decision);
            if all {
                s.auto_decision = Some(decision);
            }
        }
    }
}

/// Latest (most recently spawned) job state for the given kind, if any.
pub fn progress_snapshot(jobs: &[Job], kind: JobKind) -> Option<JobState> {
    jobs.iter()
        .rev()
        .find(|j| j.kind == kind)
        .map(|j| j.snapshot())
}
