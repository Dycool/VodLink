use crate::app::{AppController, SettingsUpdate};
use crate::web;
use anyhow::{Context, Result};
use std::sync::Arc;
use std::time::Duration;
use tao::dpi::LogicalSize;
use tao::event::{Event, WindowEvent};
use tao::event_loop::{ControlFlow, EventLoopBuilder, EventLoopProxy};
use tao::window::{Icon as WindowIcon, Window, WindowBuilder};
use tray_icon::menu::{
    CheckMenuItem, Menu, MenuEvent, MenuItem, PredefinedMenuItem,
};
use tray_icon::{
    Icon as TrayIconImage, MouseButton, MouseButtonState, TrayIcon, TrayIconBuilder, TrayIconEvent,
};
use wry::{WebView, WebViewBuilder};

enum UserEvent {
    Show,
    ShowSettings,
    Menu(MenuEvent),
    Tray(TrayIconEvent),
    TrayState {
        auto_record: bool,
        share_vods: bool,
        recording: bool,
        tooltip: String,
    },
    Shutdown,
}

pub(crate) fn run(start_minimized: bool) -> Result<()> {
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .context("Could not initialize the VodLink async runtime")?;

    if runtime.block_on(web::existing_instance(!start_minimized)) {
        return Ok(());
    }

    let listener = web::bind_ui().map_err(|error| {
        if error.kind() == std::io::ErrorKind::AddrInUse {
            anyhow::anyhow!("VodLink UI port is already occupied by another application")
        } else {
            anyhow::anyhow!("Could not bind the VodLink local UI server: {error}")
        }
    })?;

    let controller = runtime.block_on(AppController::new())?;
    let event_loop = EventLoopBuilder::<UserEvent>::with_user_event().build();
    let show_proxy = event_loop.create_proxy();
    web::register_show_window_handler(Arc::new(move || {
        let _ = show_proxy.send_event(UserEvent::Show);
    }))?;
    let exit_proxy = event_loop.create_proxy();
    web::register_exit_handler(Arc::new(move || {
        let _ = exit_proxy.send_event(UserEvent::Shutdown);
    }))?;

    let (window_icon, tray_icon_image) = load_icons()?;
    let window = WindowBuilder::new()
        .with_title("VodLink")
        .with_inner_size(LogicalSize::new(1580.0, 900.0))
        .with_min_inner_size(LogicalSize::new(1180.0, 720.0))
        .with_window_icon(Some(window_icon))
        .with_visible(!start_minimized)
        .build(&event_loop)
        .context("Could not create the VodLink desktop window")?;

    let webview = build_webview(&window, &web::ui_url())?;
    let (auto_record, share_vods, _, _) = runtime.block_on(controller.tray_state());
    let tray = build_tray(
        &event_loop.create_proxy(),
        tray_icon_image,
        auto_record,
        share_vods,
    )?;

    if start_minimized && tray.icon.is_none() {
        window.set_visible(true);
        window.set_minimized(true);
    }

    let monitor_controller = controller.clone();
    let _monitor_task = runtime.spawn(monitor_controller.run_monitor());

    let server_controller = controller.clone();
    let server_proxy = event_loop.create_proxy();
    let _server_task = runtime.spawn(async move {
        if let Err(error) = web::serve_bound(server_controller, listener).await {
            tracing::error!(%error, "VodLink local UI server stopped unexpectedly");
            let _ = server_proxy.send_event(UserEvent::Shutdown);
        }
    });

    let state_controller = controller.clone();
    let state_proxy = event_loop.create_proxy();
    let _tray_state_task = runtime.spawn(async move {
        let mut timer = tokio::time::interval(Duration::from_millis(750));
        loop {
            timer.tick().await;
            let (auto_record, share_vods, recording, tooltip) =
                state_controller.tray_state().await;
            if state_proxy
                .send_event(UserEvent::TrayState {
                    auto_record,
                    share_vods,
                    recording,
                    tooltip,
                })
                .is_err()
            {
                break;
            }
        }
    });

    let runtime_handle = runtime.handle().clone();
    let runtime_guard = runtime;
    let mut quit_requested = false;
    let mut auto_checked = auto_record;
    let mut share_checked = share_vods;
    let window_id = window.id();

    event_loop.run(move |event, _, control_flow| {
        let _runtime_guard = &runtime_guard;
        let _webview_guard = &webview;
        let _tray_guard = &tray.icon;
        *control_flow = ControlFlow::Wait;

        match event {
            Event::WindowEvent {
                window_id: event_window,
                event: WindowEvent::CloseRequested,
                ..
            } if event_window == window_id => {
                if tray.icon.is_some() && !quit_requested {
                    window.set_visible(false);
                } else if !quit_requested {
                    quit_requested = true;
                    request_quit(
                        runtime_handle.clone(),
                        controller.clone(),
                        tray.proxy.clone(),
                    );
                }
            }
            Event::UserEvent(UserEvent::Show) => {
                show_window(&window);
            }
            Event::UserEvent(UserEvent::ShowSettings) => {
                show_window(&window);
                let _ = webview.evaluate_script(
                    "document.querySelector('.nav[data-page=\"settings\"]')?.click();",
                );
            }
            Event::UserEvent(UserEvent::Tray(event)) => match event {
                TrayIconEvent::Click {
                    button: MouseButton::Left,
                    button_state: MouseButtonState::Up,
                    ..
                }
                | TrayIconEvent::DoubleClick {
                    button: MouseButton::Left,
                    ..
                } => show_window(&window),
                _ => {}
            },
            Event::UserEvent(UserEvent::Menu(event)) => {
                if event.id == tray.open_id {
                    show_window(&window);
                } else if event.id == tray.settings_id {
                    let _ = tray.proxy.send_event(UserEvent::ShowSettings);
                } else if event.id == tray.auto_id {
                    auto_checked = !auto_checked;
                    tray.auto_item.set_checked(auto_checked);
                    let update = SettingsUpdate {
                        auto_record: Some(auto_checked),
                        ..SettingsUpdate::default()
                    };
                    let update_controller = controller.clone();
                    let _ = runtime_handle.spawn(async move {
                        if let Err(error) = update_controller.update_settings(update).await {
                            tracing::error!(%error, "Could not update auto-record from tray");
                        }
                    });
                } else if event.id == tray.share_id {
                    share_checked = !share_checked;
                    tray.share_item.set_checked(share_checked);
                    let update = SettingsUpdate {
                        share_vods: Some(share_checked),
                        ..SettingsUpdate::default()
                    };
                    let update_controller = controller.clone();
                    let _ = runtime_handle.spawn(async move {
                        if let Err(error) = update_controller.update_settings(update).await {
                            tracing::error!(%error, "Could not update VOD sharing from tray");
                        }
                    });
                } else if event.id == tray.quit_id && !quit_requested {
                    quit_requested = true;
                    window.set_visible(false);
                    request_quit(
                        runtime_handle.clone(),
                        controller.clone(),
                        tray.proxy.clone(),
                    );
                }
            }
            Event::UserEvent(UserEvent::TrayState {
                auto_record,
                share_vods,
                recording,
                tooltip,
            }) => {
                auto_checked = auto_record;
                share_checked = share_vods;
                tray.auto_item.set_checked(auto_record);
                tray.auto_item.set_enabled(!recording);
                tray.share_item.set_checked(share_vods);
                if let Some(icon) = &tray.icon {
                    let label = if tooltip.trim().is_empty() {
                        "VodLink".to_owned()
                    } else {
                        format!("VodLink — {tooltip}")
                    };
                    let _ = icon.set_tooltip(Some(label));
                }
            }
            Event::UserEvent(UserEvent::Shutdown) => {
                *control_flow = ControlFlow::Exit;
            }
            _ => {}
        }
    });
}

fn request_quit(
    runtime: tokio::runtime::Handle,
    controller: Arc<AppController>,
    proxy: EventLoopProxy<UserEvent>,
) {
    let finished_proxy = proxy.clone();
    let _ = runtime.spawn(async move {
        if let Err(error) = controller.request_shutdown().await {
            tracing::error!(%error, "VodLink shutdown cleanup failed");
        }
        let _ = finished_proxy.send_event(UserEvent::Shutdown);
    });

    let _ = runtime.spawn(async move {
        tokio::time::sleep(Duration::from_secs(20)).await;
        let _ = proxy.send_event(UserEvent::Shutdown);
    });
}

fn show_window(window: &Window) {
    window.set_visible(true);
    window.set_minimized(false);
    window.set_focus();
}

fn build_webview(window: &Window, url: &str) -> Result<WebView> {
    let builder = WebViewBuilder::new().with_url(url);

    #[cfg(target_os = "linux")]
    {
        use tao::platform::unix::WindowExtUnix;
        use wry::WebViewBuilderExtUnix;

        let container = window
            .default_vbox()
            .context("VodLink could not obtain the desktop WebView container")?;
        builder
            .build_gtk(container)
            .context("Could not create the VodLink embedded WebView")
    }

    #[cfg(not(target_os = "linux"))]
    {
        builder
            .build(window)
            .context("Could not create the VodLink embedded WebView")
    }
}

struct TrayState {
    icon: Option<TrayIcon>,
    proxy: EventLoopProxy<UserEvent>,
    open_id: tray_icon::menu::MenuId,
    auto_id: tray_icon::menu::MenuId,
    share_id: tray_icon::menu::MenuId,
    settings_id: tray_icon::menu::MenuId,
    quit_id: tray_icon::menu::MenuId,
    auto_item: CheckMenuItem,
    share_item: CheckMenuItem,
}

fn build_tray(
    proxy: &EventLoopProxy<UserEvent>,
    icon: TrayIconImage,
    auto_record: bool,
    share_vods: bool,
) -> Result<TrayState> {
    let menu = Menu::new();
    let open = MenuItem::new("Open VodLink", true, None);
    let auto_item = CheckMenuItem::new("Auto-record games", true, auto_record, None);
    let share_item = CheckMenuItem::new("Share VODs with friends", true, share_vods, None);
    let settings = MenuItem::new("Settings…", true, None);
    let quit = MenuItem::new("Quit", true, None);

    menu.append(&open)?;
    menu.append(&PredefinedMenuItem::separator())?;
    menu.append(&auto_item)?;
    menu.append(&share_item)?;
    menu.append(&PredefinedMenuItem::separator())?;
    menu.append(&settings)?;
    menu.append(&PredefinedMenuItem::separator())?;
    menu.append(&quit)?;

    let open_id = open.id().clone();
    let auto_id = auto_item.id().clone();
    let share_id = share_item.id().clone();
    let settings_id = settings.id().clone();
    let quit_id = quit.id().clone();

    let menu_proxy = proxy.clone();
    MenuEvent::set_event_handler(Some(move |event| {
        let _ = menu_proxy.send_event(UserEvent::Menu(event));
    }));
    let tray_proxy = proxy.clone();
    TrayIconEvent::set_event_handler(Some(move |event| {
        let _ = tray_proxy.send_event(UserEvent::Tray(event));
    }));

    let tray_icon = match TrayIconBuilder::new()
        .with_menu(Box::new(menu))
        .with_menu_on_left_click(false)
        .with_menu_on_right_click(true)
        .with_icon(icon)
        .with_tooltip("VodLink")
        .build()
    {
        Ok(icon) => Some(icon),
        Err(error) => {
            tracing::warn!(%error, "System tray is unavailable; closing the window will quit VodLink");
            None
        }
    };

    Ok(TrayState {
        icon: tray_icon,
        proxy: proxy.clone(),
        open_id,
        auto_id,
        share_id,
        settings_id,
        quit_id,
        auto_item,
        share_item,
    })
}

fn load_icons() -> Result<(WindowIcon, TrayIconImage)> {
    let decoded = image::load_from_memory(include_bytes!("../resources/vodlink.png"))
        .context("Could not decode the embedded VodLink icon")?
        .into_rgba8();
    let (width, height) = decoded.dimensions();
    let rgba = decoded.as_raw().to_vec();

    let window_icon = WindowIcon::from_rgba(rgba.clone(), width, height)
        .map_err(|error| anyhow::anyhow!("Could not create the VodLink window icon: {error}"))?;
    let tray_icon = TrayIconImage::from_rgba(rgba, width, height)
        .map_err(|error| anyhow::anyhow!("Could not create the VodLink tray icon: {error}"))?;

    Ok((window_icon, tray_icon))
}
