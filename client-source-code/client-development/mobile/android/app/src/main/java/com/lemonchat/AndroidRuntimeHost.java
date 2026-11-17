package com.lemonchat;

/** Keeps one Node runtime across service recreation. */
final class AndroidRuntimeHost
{
	private static NodeBridge bridge;

	/**
	 * Attaches a service to the shared bridge, starting the runtime only once.
	 *
	 * @param service current service that receives native events
	 * @return the shared bridge, or {@code null} if its listening socket cannot start
	 */
	static synchronized NodeBridge attach(BackgroundService service)
	{
		if (bridge == null)
		{
			NodeBridge candidate = new NodeBridge(service);
			if (!candidate.start())
			{
				return null;
			}
			bridge = candidate;
			NodeRuntime.start(service.getApplicationContext(), bridge.getPort(), bridge.getToken());
		}
		bridge.attachService(service);
		return bridge;
	}

	/**
	 * Detaches only the matching service without stopping the embedded Node process.
	 *
	 * @param service service being destroyed
	 */
	static synchronized void detach(BackgroundService service)
	{
		if (bridge != null)
		{
			bridge.detachService(service);
		}
	}

	/**
	 * Publishes a terminal runtime failure through the existing bridge.
	 *
	 * @param reason user explanation for the failure
	 */
	static synchronized void runtimeFailed(String reason)
	{
		if (bridge != null)
		{
			bridge.reportRuntimeFailure(reason);
		}
	}
}
