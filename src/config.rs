use url::Url;

const DEFAULT_GOOGLE_CLIENT_ID: &str =
    "682238509762-ejh37d898kcl2616k8atmeo095o42plk.apps.googleusercontent.com";
const DEFAULT_WORKER_URL: &str = "https://vodlink.diogoenes0.workers.dev";

#[derive(Clone, Debug)]
pub(crate) struct Config {
    google_client_id: String,
    google_client_secret: String,
    worker_url: String,
}

impl Config {
    pub(crate) fn load() -> Self {
        let google_client_id = option_env!("VODLINK_GOOGLE_CLIENT_ID")
            .unwrap_or(DEFAULT_GOOGLE_CLIENT_ID)
            .trim()
            .to_owned();
        let google_client_secret = option_env!("VODLINK_GOOGLE_CLIENT_SECRET")
            .unwrap_or("")
            .trim()
            .to_owned();
        let worker_url = option_env!("VODLINK_WORKER_URL")
            .unwrap_or(DEFAULT_WORKER_URL)
            .trim()
            .trim_end_matches('/')
            .to_owned();
        Self { google_client_id, google_client_secret, worker_url }
    }

    pub(crate) fn google_client_id(&self) -> &str { &self.google_client_id }
    pub(crate) fn google_client_secret(&self) -> &str { &self.google_client_secret }
    pub(crate) fn worker_url(&self) -> &str { &self.worker_url }

    pub(crate) fn worker_configured(&self) -> bool {
        !self.worker_url.is_empty() && Url::parse(&self.worker_url).is_ok()
    }
}
