// Copyright 2020 Oath Inc. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
package ai.vespa.metricsproxy.http.application;

import ai.vespa.metricsproxy.metric.model.ConsumerId;
import ai.vespa.metricsproxy.metric.model.MetricsPacket;
import ai.vespa.metricsproxy.metric.model.json.GenericJsonUtil;
import ai.vespa.metricsproxy.metric.model.processing.MetricsProcessor;
import com.yahoo.yolean.Exceptions;
import org.apache.http.client.HttpClient;
import org.apache.http.client.methods.HttpGet;
import org.apache.http.impl.client.BasicResponseHandler;

import java.io.IOException;
import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.logging.Logger;

import static ai.vespa.metricsproxy.metric.model.processing.MetricsProcessor.applyProcessors;
import static java.util.Collections.emptyList;
import static java.util.logging.Level.FINE;
import static java.util.stream.Collectors.toList;

/**
 * Retrieves metrics from a single Vespa node over http. To avoid unnecessary load on metrics
 * proxies, a cached snapshot per consumer is retained and served for a fixed TTL period.
 * Upon failure to retrieve metrics, an empty snapshot is cached.
 *
 * This class assumes that the consumer id is a valid and existing one, which is already
 * ensured by the {@link ApplicationMetricsHandler}.
 *
 * @author gjoranv
 */
public class NodeMetricsClient {

    private static final Logger log = Logger.getLogger(NodeMetricsClient.class.getName());

    static final Duration METRICS_TTL = Duration.ofSeconds(30);
    private static final int MAX_DIMENSIONS = 10;

    final Node node;
    private final HttpClient httpClient;
    private final Clock clock;

    private final Map<ConsumerId, Snapshot> snapshots = new ConcurrentHashMap<>();
    private long snapshotsRetrieved = 0;

    NodeMetricsClient(HttpClient httpClient, Node node, Clock clock) {
        this.httpClient = httpClient;
        this.node = node;
        this.clock = clock;
    }

    public List<MetricsPacket> getMetrics(ConsumerId consumer) {
        var currentSnapshot = snapshots.get(consumer);
        if (currentSnapshot == null || currentSnapshot.isStale(clock) || currentSnapshot.metrics.isEmpty()) {
            Snapshot snapshot = retrieveMetrics(consumer);
            snapshots.put(consumer, snapshot);
            return snapshot.metrics;
        } else {
            return snapshots.get(consumer).metrics;
        }
    }

    private Snapshot retrieveMetrics(ConsumerId consumer) {
        String metricsUri = node.metricsUri(consumer).toString();
        log.log(FINE, () -> "Retrieving metrics from host " + metricsUri);

        try {
            String metricsJson = httpClient.execute(new HttpGet(metricsUri), new BasicResponseHandler());
            var metricsBuilders = GenericJsonUtil.toMetricsPackets(metricsJson);
            var metrics = processAndBuild(metricsBuilders,
                                          new ServiceIdDimensionProcessor(),
                                          new ClusterIdDimensionProcessor(),
                                          new PublicDimensionsProcessor(MAX_DIMENSIONS));
            snapshotsRetrieved ++;
            log.log(FINE, () -> "Successfully retrieved " + metrics.size() + " metrics packets from " + metricsUri);

            return new Snapshot(Instant.now(clock), metrics);
        } catch (IOException e) {
            log.warning("Unable to retrieve metrics from " + metricsUri + ": " + Exceptions.toMessageString(e));
            return new Snapshot(Instant.now(clock), emptyList());
        }
    }

    private static List<MetricsPacket> processAndBuild(List<MetricsPacket.Builder> builders,
                                                       MetricsProcessor... processors) {
        return builders.stream()
                    .map(builder -> applyProcessors(builder, processors))
                    .map(MetricsPacket.Builder::build)
                    .collect(toList());
    }

    long snapshotsRetrieved() {
        return snapshotsRetrieved;
    }

    /**
     * Convenience class for storing a metrics snapshot with its timestamp.
     */
    static class Snapshot {

        final Instant timestamp;
        final List<MetricsPacket> metrics;

        Snapshot(Instant timestamp, List<MetricsPacket> metrics) {
            this.timestamp = timestamp;
            this.metrics = metrics;
        }

        boolean isStale(Clock clock) {
            return Instant.now(clock).isAfter(timestamp.plus(METRICS_TTL));
        }
    }

}
