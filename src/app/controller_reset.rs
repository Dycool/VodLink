impl AppController {
    pub(crate) async fn factory_reset_local_data(&self) -> Result<()> {
        self.set_message("Resetting VodLink and deleting local data…")
            .await;
        self.explicitly_signed_out.store(true, Ordering::Release);

        self.stop_recording().await?;
        *self.tokens.write().await = AuthTokens::default();

        self.paths.schedule_reset_after_exit()?;
        self.set_message("VodLink local data reset. Closing…").await;
        self.shutdown_requested.store(true, Ordering::Release);
        self.shutdown_notify.notify_waiters();
        Ok(())
    }
}
