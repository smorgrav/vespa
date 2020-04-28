// Copyright Verizon Media. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
package com.yahoo.vespa.hosted.provision.autoscale;

import com.yahoo.config.provision.Capacity;
import com.yahoo.config.provision.ClusterResources;
import com.yahoo.config.provision.NodeResources;
import com.yahoo.vespa.hosted.provision.applications.Cluster;

/**
 * Optional allocation limits
 *
 * @author bratseth
 */
public class Limits {

    private static final Limits empty = new Limits(null, null);

    private final ClusterResources min, max;

    private Limits(ClusterResources min, ClusterResources max) {
        this.min = min;
        this.max = max;
    }

    public static Limits empty() { return empty; }

    public boolean isEmpty() { return this == empty; }

    public ClusterResources min() {
        if (isEmpty()) throw new IllegalStateException("Empty: No min");
        return min;
    }

    public ClusterResources max() {
        if (isEmpty()) throw new IllegalStateException("Empty: No max");
        return max;
    }

    /** Caps the given resources at the limits of this. If it is empty the node resources are returned as-is */
    public NodeResources cap(NodeResources resources) {
        if (isEmpty()) return resources;
        resources = resources.withVcpu(between(min.nodeResources().vcpu(), max.nodeResources().vcpu(), resources.vcpu()));
        resources = resources.withMemoryGb(between(min.nodeResources().memoryGb(), max.nodeResources().memoryGb(), resources.memoryGb()));
        resources = resources.withDiskGb(between(min.nodeResources().diskGb(), max.nodeResources().diskGb(), resources.diskGb()));
        return resources;
    }

    private double between(double min, double max, double value) {
        value = Math.max(min, value);
        value = Math.min(max, value);
        return value;
    }

    public static Limits of(Cluster cluster) {
        return new Limits(cluster.minResources(), cluster.maxResources());
    }

    public static Limits of(Capacity capacity) {
        return new Limits(capacity.minResources(), capacity.maxResources());
    }

    @Override
    public String toString() {
        if (isEmpty()) return "no limits";
        return "limits: from " + min + " to " + max;
    }

}
