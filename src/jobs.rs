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
}

/// A background job running on a worker thread with progress and cancellation.
pub struct Job {
    pub kind: JobKind,
    pub label: String,
    state: Arc<Mutex<JobState>>,
    cancel: Arc<AtomicBool>,
    #[allow(dead_code)]
    handle: Option<JoinHandle<()>>,
}

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
}

/// Latest (most recently spawned) job state for the given kind, if any.
pub fn progress_snapshot(jobs: &[Job], kind: JobKind) -> Option<JobState> {
    jobs.iter()
        .rev()
        .find(|j| j.kind == kind)
        .map(|j| j.snapshot())
}
