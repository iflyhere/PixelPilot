package com.openipc.pixelpilot;

/**
 * Where {@link WfbLinkManager} reports adapter/link status to. The flat activity draws it
 * into its overlay; the immersive activity has no view hierarchy and logs it instead.
 */
public interface LinkStatusView {
    /** Adapter or link state the pilot needs to see (starting, no permission, ...). */
    void showLinkMessage(String message);

    /** Shown when no compatible adapter is attached: where to push a stream instead. */
    void showLocalStreamHint(String url);
}
