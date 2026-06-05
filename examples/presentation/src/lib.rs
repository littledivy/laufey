use laufey::Window;

fn presentation_main() {
  let mut args = std::env::args().skip(1);
  let mut url = None;
  while let Some(arg) = args.next() {
    if arg == "--runtime" {
      args.next(); // skip value
    } else if arg.starts_with("--runtime=") {
      // skip
    } else if arg.starts_with("--") {
      // skip other flags
    } else {
      url = Some(arg);
      break;
    }
  }
  let url = url.unwrap_or_else(|| {
    eprintln!("Usage: laufey-presentation <google-slides-url>");
    std::process::exit(1);
  });

  // Convert Google Slides URLs to embed format for clean presentation view
  let url = if url.contains("docs.google.com/presentation")
    && !url.contains("/embed")
  {
    let id = url
      .split("/d/")
      .nth(1)
      .and_then(|s| s.split('/').next())
      .unwrap_or(&url);
    format!(
      "https://docs.google.com/presentation/d/{}/embed?start=false&loop=false",
      id
    )
  } else {
    url
  };

  let rt = tokio::runtime::Runtime::new().unwrap();
  rt.block_on(async {
    let _win = Window::new(1280, 720)
      .title("LAUFEY - Presentation")
      .load(&url);

    laufey::run().await;
  });
}

laufey::main!(presentation_main);
