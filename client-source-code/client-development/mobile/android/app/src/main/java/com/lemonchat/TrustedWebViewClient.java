package com.lemonchat;

import android.content.Intent;
import android.net.Uri;
import android.webkit.WebResourceRequest;
import android.webkit.WebView;
import android.webkit.WebViewClient;

/** Limits native access to the packaged page. */
final class TrustedWebViewClient extends WebViewClient
{
	static final String DOCUMENT = "file:///android_asset/client.html";
	private final BackgroundService service;

	/**
	 * Uses the service that owns the WebView.
	 * @param service owner of page callbacks
	 */
	TrustedWebViewClient(BackgroundService service)
	{
		this.service = service;
	}

	/**
	 * Accepts the packaged page and its fragment URLs.
	 * @param url URL to check
	 * @return whether the URL is trusted
	 */
	static boolean trusted(String url)
	{
		return url != null && (url.equals(DOCUMENT) || url.startsWith(DOCUMENT + "#"));
	}

	/**
	 * Blocks subframes and checks main page navigation.
	 * @param view requesting WebView
	 * @param request requested destination
	 * @return {@code true} when navigation is handled or blocked
	 */
	@Override public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request)
	{
		if (!request.isForMainFrame())
		{
			return true;
		}
		return this.navigate(request.getUrl().toString());
	}

	/**
	 * Checks navigation on older WebViews.
	 * @param view requesting WebView
	 * @param url requested destination
	 * @return {@code true} when navigation is handled or blocked
	 */
	@Override public boolean shouldOverrideUrlLoading(WebView view, String url)
	{
		return this.navigate(url);
	}

	/**
	 * Opens external HTTP links outside the WebView and blocks other untrusted URLs.
	 * @param url requested destination
	 * @return {@code false} only for the trusted page
	 */
	private boolean navigate(String url)
	{
		if (trusted(url))
		{
			return false;
		}
		Uri uri = Uri.parse(url);
		if ("https".equals(uri.getScheme()) || "http".equals(uri.getScheme()))
		{
			try
			{
				Intent intent = new Intent(Intent.ACTION_VIEW, uri);
				intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
				this.service.startActivity(intent);
			}
			catch (RuntimeException unavailable)
			{
			}
		}
		return true;
	}

	/**
	 * Starts page tracking and blocks untrusted loads.
	 * @param view loading WebView
	 * @param url destination being loaded
	 * @param favicon page icon, possibly {@code null}
	 */
	@Override public void onPageStarted(WebView view, String url, android.graphics.Bitmap favicon)
	{
		this.service.beginPage(url);
		if (!trusted(url))
		{
			view.removeJavascriptInterface("Android");
			view.stopLoading();
		}
	}

	/**
	 * Invalidates the page when its main document fails to load.
	 * @param view affected WebView
	 * @param request failed request
	 * @param error failure details
	 */
	@Override public void onReceivedError(WebView view, WebResourceRequest request, android.webkit.WebResourceError error)
	{
		if (request.isForMainFrame())
		{
			this.service.invalidatePage();
		}
	}

}
