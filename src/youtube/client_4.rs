impl YouTubeLiveClient {
    async fn request(
        &self,
        method: Method,
        path: &str,
        query: &[(&str, &str)],
        token: &str,
        body: Option<Value>,
    ) -> Result<Value> {
        if token.trim().is_empty() {
            bail!("Google sign-in is required for this YouTube request");
        }
        let mut request = self
            .client
            .request(method, format!("{API_BASE}{path}"))
            .bearer_auth(token)
            .query(query);
        if let Some(body) = body {
            request = request.json(&body);
        }
        let response = request.send().await?;
        let status = response.status();
        let text = response.text().await?;
        if !status.is_success() {
            let parsed = serde_json::from_str::<Value>(&text)
                .unwrap_or_else(|_| json!({"error":{"message":text}}));
            let message = api_error_message(&parsed);
            if status == StatusCode::UNAUTHORIZED {
                bail!("AUTH_EXPIRED: {message}");
            }
            if status == StatusCode::FORBIDDEN && is_quota_error(&parsed) {
                bail!("YouTube API quota/rate limit reached: {message}");
            }
            bail!("YouTube API request failed ({status}): {message}");
        }
        if text.trim().is_empty() {
            Ok(json!({}))
        } else {
            serde_json::from_str(&text).context("YouTube returned invalid JSON")
        }
    }
}
