//! Independent tasks: scoped threads natively, one after another on WebAssembly (it has no
//! threads). Each task writes only its own slice of the output, so the order never changes a result.
pub type Task<'a> = Box<dyn FnOnce() + Send + 'a>;

#[cfg(not(target_arch = "wasm32"))]
pub fn run(tasks: Vec<Task<'_>>) {
    std::thread::scope(|scope| {
        for task in tasks {
            scope.spawn(task);
        }
    });
}

#[cfg(target_arch = "wasm32")]
pub fn run(tasks: Vec<Task<'_>>) {
    for task in tasks {
        task();
    }
}
