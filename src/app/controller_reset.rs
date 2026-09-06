impl AppController {
    pub(crate) async fn factory_reset_local_data(&self) -> Result<()> {
        self.explicitly_signed_out.store(true, Ordering::Release);

        // C++ treats active-stream cleanup as best-effort during a factory
        // reset: cleanup is attempted first, but reset still proceeds even if
        // finalization fails so broken streaming state cannot block deletion of
        // local VodLink data. It also does not publish transient global status
        // messages during this reset-and-close path.
        if let Err(error) = self.stop_recording().await {
            tracing::warn!(%error, "Stream cleanup failed while resetting local data");
        }
        *self.tokens.write().await = AuthTokens::default();

        self.paths.schedule_reset_after_exit()?;
        self.shutdown_requested.store(true, Ordering::Release);
        self.shutdown_notify.notify_waiters();
        Ok(())
    }
}
