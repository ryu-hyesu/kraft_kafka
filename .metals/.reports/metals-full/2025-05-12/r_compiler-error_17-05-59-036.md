file://<WORKSPACE>/clients/src/main/java/org/apache/kafka/clients/consumer/internals/AbstractFetch.java
### java.util.NoSuchElementException: next on empty iterator

occurred in the presentation compiler.

presentation compiler configuration:


action parameters:
offset: 10229
uri: file://<WORKSPACE>/clients/src/main/java/org/apache/kafka/clients/consumer/internals/AbstractFetch.java
text:
```scala
/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements. See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package org.apache.kafka.clients.consumer.internals;

import java.io.Closeable;
import java.time.Duration;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.function.Predicate;
import java.util.stream.Collectors;

import org.apache.kafka.clients.ApiVersions;
import org.apache.kafka.clients.ClientResponse;
import org.apache.kafka.clients.FetchSessionHandler;
import org.apache.kafka.clients.KafkaClient;
import org.apache.kafka.clients.Metadata;
import org.apache.kafka.clients.NetworkClientUtils;
import org.apache.kafka.clients.consumer.ConsumerRecord;
import static org.apache.kafka.clients.consumer.internals.FetchUtils.requestMetadataUpdate;
import org.apache.kafka.common.Cluster;
import org.apache.kafka.common.Node;
import org.apache.kafka.common.TopicPartition;
import org.apache.kafka.common.Uuid;
import org.apache.kafka.common.errors.AuthenticationException;
import org.apache.kafka.common.internals.IdempotentCloser;
import org.apache.kafka.common.message.FetchResponseData;
import org.apache.kafka.common.protocol.ApiKeys;
import org.apache.kafka.common.protocol.Errors;
import org.apache.kafka.common.requests.FetchRequest;
import org.apache.kafka.common.requests.FetchResponse;
import org.apache.kafka.common.utils.BufferSupplier;
import org.apache.kafka.common.utils.LogContext;
import org.apache.kafka.common.utils.Time;
import org.apache.kafka.common.utils.Timer;
import org.apache.kafka.common.utils.Utils;
import org.slf4j.Logger;
import org.slf4j.helpers.MessageFormatter;

/**
 * {@code AbstractFetch} represents the basic state and logic for record fetching processing.
 */
public abstract class AbstractFetch implements Closeable {

    private final Logger log;
    private final IdempotentCloser idempotentCloser = new IdempotentCloser();
    protected final LogContext logContext;
    protected final ConsumerMetadata metadata;
    protected final SubscriptionState subscriptions;
    protected final FetchConfig fetchConfig;
    protected final Time time;
    protected final FetchMetricsManager metricsManager;
    protected final FetchBuffer fetchBuffer;
    protected final BufferSupplier decompressionBufferSupplier;
    protected final Set<Integer> nodesWithPendingFetchRequests;

    private final Map<Integer, FetchSessionHandler> sessionHandlers;

    private final ApiVersions apiVersions;

    public AbstractFetch(final LogContext logContext,
                         final ConsumerMetadata metadata,
                         final SubscriptionState subscriptions,
                         final FetchConfig fetchConfig,
                         final FetchBuffer fetchBuffer,
                         final FetchMetricsManager metricsManager,
                         final Time time,
                         final ApiVersions apiVersions) {
        this.log = logContext.logger(AbstractFetch.class);
        this.logContext = logContext;
        this.metadata = metadata;
        this.subscriptions = subscriptions;
        this.fetchConfig = fetchConfig;
        this.fetchBuffer = fetchBuffer;
        this.decompressionBufferSupplier = BufferSupplier.create();
        this.sessionHandlers = new HashMap<>();
        this.nodesWithPendingFetchRequests = new HashSet<>();
        this.metricsManager = metricsManager;
        this.time = time;
        this.apiVersions = apiVersions;
    }

    /**
     * Check if the node is disconnected and unavailable for immediate reconnection (i.e. if it is in
     * reconnect backoff window following the disconnect).
     *
     * @param node {@link Node} to check for availability
     * @see NetworkClientUtils#isUnavailable(KafkaClient, Node, Time)
     */
    protected abstract boolean isUnavailable(Node node);

    /**
     * Checks for an authentication error on a given node and throws the exception if it exists.
     *
     * @param node {@link Node} to check for a previous {@link AuthenticationException}; if found it is thrown
     * @see NetworkClientUtils#maybeThrowAuthFailure(KafkaClient, Node)
     */
    protected abstract void maybeThrowAuthFailure(Node node);

    /**
     * Return whether we have any completed fetches pending return to the user. This method is thread-safe. Has
     * visibility for testing.
     *
     * @return true if there are completed fetches, false otherwise
     */
    boolean hasCompletedFetches() {
        return !fetchBuffer.isEmpty();
    }

    /**
     * Return whether we have any completed fetches that are fetchable. This method is thread-safe.
     * @return true if there are completed fetches that can be returned, false otherwise
     */
    public boolean hasAvailableFetches() {
        return fetchBuffer.hasCompletedFetches(fetch -> subscriptions.isFetchable(fetch.partition));
    }

    /**
     * Implements the core logic for a successful fetch response.
     *
     * @param fetchTarget {@link Node} from which the fetch data was requested
     * @param data {@link FetchSessionHandler.FetchRequestData} that represents the session data
     * @param resp {@link ClientResponse} from which the {@link FetchResponse} will be retrieved
     */
    protected void handleFetchSuccess(final Node fetchTarget,
                                      final FetchSessionHandler.FetchRequestData data,
                                      final ClientResponse resp) {
        try {
            final FetchResponse response = (FetchResponse) resp.responseBody();
            final FetchSessionHandler handler = sessionHandler(fetchTarget.id()); // node에 대한 fetch 세션 헨들러 불러오기기

            if (handler == null) {
                log.error("Unable to find FetchSessionHandler for node {}. Ignoring fetch response.",
                        fetchTarget.id());
                return;
            }

            final short requestVersion = resp.requestHeader().apiVersion();

            if (!handler.handleResponse(response, requestVersion)) {
                if (response.error() == Errors.FETCH_SESSION_TOPIC_ID_ERROR) {
                    metadata.requestUpdate(false);
                }

                return;
            }

            // response body에서 각 파티션에 대한 데이터를 parsing
            // sessionTopicNames에서 세션에 등록된 토픽 이름을 불러옴옴
            final Map<TopicPartition, FetchResponseData.PartitionData> responseData = response.responseData(handler.sessionTopicNames(), requestVersion);
            final Set<TopicPartition> partitions = new HashSet<>(responseData.keySet());
            final FetchMetricsAggregator metricAggregator = new FetchMetricsAggregator(metricsManager, partitions);

            Map<TopicPartition, Metadata.LeaderIdAndEpoch> partitionsWithUpdatedLeaderInfo = new HashMap<>();
            // 파티션 반복 처리 및 CompletedFetch 생성하여
            // 요청했던 offset과 응답을 매칭함함
            // 이때 CompletedFetch은 fetch 결과의 단위를 의미함함
            for (Map.Entry<TopicPartition, FetchResponseData.PartitionData> entry : responseData.entrySet()) {
                TopicPartition partition = entry.getKey();

                // (1) 요청 정보 검증 
                // 이는 응답으로 온 파티션이 요청 시점에 포함되었는지 검증함
                FetchRequest.PartitionData requestData = data.sessionPartitions().get(partition);

                if (requestData == null) {
                    String message;

                    if (data.metadata().isFull()) {
                        message = MessageFormatter.arrayFormat(
                                "Response for missing full request partition: partition={}; metadata={}",
                                new Object[]{partition, data.metadata()}).getMessage();
                    } else {
                        message = MessageFormatter.arrayFormat(
                                "Response for missing session request partition: partition={}; metadata={}; toSend={}; toForget={}; toReplace={}",
                                new Object[]{partition, data.metadata(), data.toSend(), data.toForget(), data.toReplace()}).getMessage();
                    }

                    // Received fetch response for missing session partition
                    throw new IllegalStateException(message);
                }

                // (2) 요청 당시 오프셋과 응답 데이터를 매핑함
                long fetchOffset = requestData.fetchOffset;
                FetchResponseData.PartitionData partitionData = entry.getValue(); // 레코드, 에러코드, 마지막 오프셋 정보 등등

                log.debug("Fetch {} at offset {} for partition {} returned fetch data {}",
                        fetchConfig.isolationLevel, fetchOffset, partition, partitionData);

                // (3) 리더 오류 처리
                Errors partitionError = Errors.forCode(partitionData.errorCode());
                if (partitionError == Errors.NOT_LEADER_OR_FOLLOWER || partitionError == Errors.FENCED_LEADER_EPOCH) {
                    log.debug("For {}, received error {}, with leaderIdAndEpoch {}", partition, partitionError, partitionData.currentLeader());
                    if (partitionData.currentLeader().leaderId() != -1 && partitionData.currentLeader().leaderEpoch() != -1) {
                        partitionsWithUpdatedLeaderInfo.put(partition, new Metadata.LeaderIdAndEpoch(
                            Optional.of(partitionData.currentLeader().leaderId()), Optional.of(partitionData.currentLeader().leaderEpoch())));
                    }
                }

                // (4@@)
                CompletedFetch completedFetch = new CompletedFetch(
                        logContext,
                        subscriptions,
                        decompressionBufferSupplier,
                        partition,
                        partitionData,
                        metricAggregator,
                        fetchOffset,
                        requestVersion);
                fetchBuffer.add(completedFetch);
            }

            // 일부 파티션에서 리더 정보가 변경되었을 경우 메타 데이터에 반영함
            // 리더가 바뀌면 포지션 재검증증
            if (!partitionsWithUpdatedLeaderInfo.isEmpty()) {
                List<Node> leaderNodes = response.data().nodeEndpoints().stream()
                    .map(e -> new Node(e.nodeId(), e.host(), e.port(), e.rack()))
                    .filter(e -> !e.equals(Node.noNode()))
                    .collect(Collectors.toList());
                Set<TopicPartition> updatedPartitions = metadata.updatePartitionLeadership(partitionsWithUpdatedLeaderInfo, leaderNodes);
                updatedPartitions.forEach(
                    tp -> {
                        log.debug("For {}, as the leader was updated, position will be validated.", tp);
                        subscriptions.maybeValidatePositionForCurrentLeader(apiVersions, tp, metadata.currentLeader(tp));
                    }
                );
            }

            // fetch latency를 metric에 기록함함
            metricsManager.recordLatency(resp.destination(), resp.requestLatencyMs());
        } finally {
            // fetch에 대한 요청을 pending 목록에서 제거함함
            removePendingFetchRequest(fetchTarget, data.metadata().sessionId());
        }
    }

    /**
     * Implements the core logic for a failed fetch response.
     *
     * @param fetchTarget {@link Node} from which the fetch data was requested
     * @param data        {@link FetchSessionHandler.FetchRequestData} from request
     * @param t           {@link Throwable} representing the error that resulted in the failure
     */
    protected void handleFetchFailure(final Node fetchTarget,
                                      final FetchSessionHandler.FetchRequestData data,
                                      final Throwable t) {
        try {
            final FetchSessionHandler handler = sessionHandler(fetchTarget.id());

            if (handler != null) {
                handler.handleError(t);
                handler.sessionTopicPartitions().forEach(subscriptions::clearPreferredReadReplica);
            }
        } finally {
            removePendingFetchRequest(fetchTarget, data.metadata().sessionId());
        }
    }

    protected void handleCloseFetchSessionSuccess(final Node fetchTarget,
                                                  final FetchSessionHandler.FetchRequestData data,
                                                  final ClientResponse ignored) {
        int sessionId = data.metadata().sessionId();
        removePendingFetchRequest(fetchTarget, sessionId);
        log.debug("Successfully sent a close message for fetch session: {} to node: {}", sessionId, fetchTarget);
    }

    public void handleCloseFetchSessionFailure(final Node fetchTarget,
                                               final FetchSessionHandler.FetchRequestData data,
                                               final Throwable t) {
        int sessionId = data.metadata().sessionId();
        removePendingFetchRequest(fetchTarget, sessionId);
        log.debug("Unable to send a close message for fetch session: {} to node: {}. " +
                "This may result in unnecessary fetch sessions at the broker.", sessionId, fetchTarget, t);
    }

    private void removePendingFetchRequest(Node fetchTarget, int sessionId) {
        log.debug("Removing pending request for fetch session: {} for node: {}", sessionId, fetchTarget);
        nodesWithPendingFetchRequests.remove(fetchTarget.id());
    }

    protected void removeShmPendingFetchRequest(Node fetchTarget) {
        nodesWithPendingFetchRequests.remove(fetchTarget.id());
    }

    protected <K, V> void removePendingShmFetchRequest(Node fetchTarget,
                                            TopicPartition tp,
                                            List<ConsumerRecord<K, V>> partRecords) {
        if (!subscriptions.isAssigned(tp)) {
            log.debug("Not returning fetched records for partition {} since it is no longer assigned", tp);
        } else if (!subscriptions.isFetchable(tp)) {
            log.debug("Not returning fetched records for assigned partition {} since it is no longer fetchable", tp);
        } else {
            SubscriptionState.FetchPosition position = subscriptions.position(tp);
            if (position == null)
                throw new IllegalStateException("Missing position for fetchable partition " + tp);

            if (!partRecords.isEmpty()) {
                long lastOffset = partRecords.get(partRecords.size() - 1).offset();
                long nextOffset = lastOffset + 1;

                if (nextOffset > position.offset) {
                    SubscriptionState.FetchPosition nextPosition = new SubscriptionState.FetchPosition(
                            nextOffset,
                            Optional.ofNullable(partRecords.get(partRecords.size() - 1).leaderEpoch()).orElse(null),
                            position.currentLeader);

                    subscriptions.position(tp, nextPosition);
                }

                Long partitionLag = subscriptions.partitionLag(tp, fetchConfig.isolationLevel);
                if (partitionLag != null)
                    metricsManager.recordPartitionLag(tp, partitionLag);

                Long lead = subscriptions.partitionLead(tp);
                if (lead != null)
                    metricsManager.recordPartitionLead(tp, lead);
            }
        }

        nodesWithPendingFetchRequests.remove(fetchTarget.id());
    }



    /**
     * Creates a new {@link FetchRequest fetch request} in preparation for sending to the Kafka cluster.
     *
     * @param fetchTarget {@link Node} from which the fetch data will be requested
     * @param requestData {@link FetchSessionHandler.FetchRequestData} that represents the session data
     * @return {@link FetchRequest.Builder} that can be submitted to the broker
     */
    protected FetchRequest.Builder createFetchRequest(final Node fetchTarget,
                                                      final FetchSessionHandler.FetchRequestData requestData) {
        // Version 12 is the maximum version that could be used without topic IDs. See FetchRequest.json for schema
        // changelog.
        final short maxVersion = requestData.canUseTopicIds() ? ApiKeys.FETCH.latestVersion() : (short) 12;

        final FetchRequest.Builder request = FetchRequest.Builder
                .forConsumer(maxVersion, fetchConfig.maxWaitMs, fetchConfig.minBytes, requestData.toSend())
                .isolationLevel(fetchConfig.isolationLevel)
                .setMaxBytes(fetchConfig.maxBytes)
                .metadata(requestData.metadata())
                .removed(requestData.toForget())
                .replaced(requestData.toReplace())
                .rackId(fetchConfig.clientRackId);

        log.debug("Sending {} {} to broker {}", fetchConfig.isolationLevel, requestData, fetchTarget);

        // We add the node to the set of nodes with pending fetch requests before adding the
        // listener because the future may have been fulfilled on another thread (e.g. during a
        // disconnection being handled by the heartbeat thread) which will mean the listener
        // will be invoked synchronously.
        log.debug("Adding pending request for node {}", fetchTarget);
        nodesWithPendingFetchRequests.add(fetchTarget.id());

        return request;
    }

    /**
     * Return the list of <em>fetchable</em> partitions, which are the set of partitions to which we are subscribed,
     * but <em>excluding</em> any partitions for which we still have buffered data. The idea is that since the user
     * has yet to process the data for the partition that has already been fetched, we should not go send for more data
     * until the previously-fetched data has been processed.
     *
     * @return {@link Set} of {@link TopicPartition topic partitions} for which we should fetch data
     */
    private Set<TopicPartition> fetchablePartitions() {
        // This is the set of partitions we have in our buffer
        Set<TopicPartition> buffered = fetchBuffer.bufferedPartitions();

        // This is the test that returns true if the partition is *not* buffered
        Predicate<TopicPartition> isNotBuffered = tp -> !buffered.contains(tp);

        // Return all partitions that are in an otherwise fetchable state *and* for which we don't already have some
        // messages sitting in our buffer.
        return new HashSet<>(subscriptions.fetchablePartitions(isNotBuffered));
    }

    /**
     * Determine from which replica to read: the <i>preferred</i> or the <i>leader</i>. The preferred replica is used
     * iff:
     *
     * <ul>
     *     <li>A preferred replica was previously set</li>
     *     <li>We're still within the lease time for the preferred replica</li>
     *     <li>The replica is still online/available</li>
     * </ul>
     *
     * If any of the above are not met, the leader node is returned.
     *
     * @param partition {@link TopicPartition} for which we want to fetch data
     * @param leaderReplica {@link Node} for the leader of the given partition
     * @param currentTimeMs Current time in milliseconds; used to determine if we're within the optional lease window
     * @return Replica {@link Node node} from which to request the data
     * @see SubscriptionState#preferredReadReplica
     * @see SubscriptionState#updatePreferredReadReplica
     */
    Node selectReadReplica(final TopicPartition partition, final Node leaderReplica, final long currentTimeMs) {
        Optional<Integer> nodeId = subscriptions.preferredReadReplica(partition, currentTimeMs);

        if (nodeId.isPresent()) {
            Optional<Node> node = nodeId.flatMap(id -> metadata.fetch().nodeIfOnline(partition, id));
            if (node.isPresent()) {
                return node.get();
            } else {
                log.trace("Not fetching from {} for partition {} since it is marked offline or is missing from our metadata," +
                        " using the leader instead.", nodeId, partition);
                // Note that this condition may happen due to stale metadata, so we clear preferred replica and
                // refresh metadata.
                requestMetadataUpdate(metadata, subscriptions, partition);
                return leaderReplica;
            }
        } else {
            return leaderReplica;
        }
    }

    protected Map<Node, FetchSessionHandler.FetchRequestData> prepareCloseFetchSessionRequests() {
        final Cluster cluster = metadata.fetch();
        Map<Node, FetchSessionHandler.Builder> fetchable = new HashMap<>();

        sessionHandlers.forEach((fetchTargetNodeId, sessionHandler) -> {
            // set the session handler to notify close. This will set the next metadata request to send close message.
            sessionHandler.notifyClose();

            // FetchTargetNode may not be available as it may have disconnected the connection. In such cases, we will
            // skip sending the close request.
            final Node fetchTarget = cluster.nodeById(fetchTargetNodeId);

            if (fetchTarget == null || isUnavailable(fetchTarget)) {
                log.debug("Skip sending close session request to broker {} since it is not reachable", fetchTarget);
                return;
            }

            fetchable.put(fetchTarget, sessionHandler.newBuilder());
        });

        return fetchable.entrySet().stream().collect(Collectors.toMap(Map.Entry::getKey, e -> e.getValue().build()));
    }

    /**
     * Create fetch requests for all nodes for which we have assigned partitions
     * that have no existing requests in flight.
     */
    protected Map<Node, FetchSessionHandler.FetchRequestData> prepareFetchRequests() {
        // Update metrics in case there was an assignment change
        metricsManager.maybeUpdateAssignment(subscriptions);

        Map<Node, FetchSessionHandler.Builder> fetchable = new HashMap<>();
        long currentTimeMs = time.milliseconds();
        Map<String, Uuid> topicIds = metadata.topicIds();

        for (TopicPartition partition : fetchablePartitions()) {
            SubscriptionState.FetchPosition position = subscriptions.position(partition);

            if (position == null)
                throw new IllegalStateException("Missing position for fetchable partition " + partition);

            Optional<Node> leaderOpt = position.currentLeader.leader;

            if (leaderOpt.isEmpty()) {
                log.debug("Requesting metadata update for partition {} since the position {} is missing the current leader node", partition, position);
                metadata.requestUpdate(false);
                continue;
            }

            // Use the preferred read replica if set, otherwise the partition's leader
            Node node = selectReadReplica(partition, leaderOpt.get(), currentTimeMs);

            if (isUnavailable(node)) {
                maybeThrowAuthFailure(node);

                // If we try to send during the reconnect backoff window, then the request is just
                // going to be failed anyway before being sent, so skip sending the request for now
                log.trace("Skipping fetch for partition {} because node {} is awaiting reconnect backoff", partition, node);
            } else if (nodesWithPendingFetchRequests.contains(node.id())) {
                log.trace("Skipping fetch for partition {} because previous request to {} has not been processed", partition, node);
            } else {
                // if there is a leader and no in-flight requests, issue a new fetch
                FetchSessionHandler.Builder builder = fetchable.computeIfAbsent(node, k -> {
                    FetchSessionHandler fetchSessionHandler = sessionHandlers.computeIfAbsent(node.id(), n -> new FetchSessionHandler(logContext, n));
                    return fetchSessionHandler.newBuilder();
                });
                Uuid topicId = topicIds.getOrDefault(partition.topic(), Uuid.ZERO_UUID);
                FetchRequest.PartitionData partitionData = new FetchRequest.PartitionData(topicId,
                        position.offset,
                        FetchRequest.INVALID_LOG_START_OFFSET,
                        fetchConfig.fetchSize,
                        position.currentLeader.epoch,
                        Optional.empty());
                builder.add(partition, partitionData);

                log.debug("Added {} fetch request for partition {} at position {} to node {}", fetchConfig.isolationLevel,
                        partition, position, node);
            }
        }

        return fetchable.entrySet().stream().collect(Collectors.toMap(Map.Entry::getKey, e -> e.getValue().build()));
    }

    // Visible for testing
    protected FetchSessionHandler sessionHandler(int node) {
        return sessionHandlers.get(node);
    }

    /**
     * This method is called by {@link #close(Timer)} which is guarded by the {@link IdempotentCloser}) such as to only
     * be executed once the first time that any of the {@link #close()} methods are called. Subclasses can override
     * this method without the need for extra synchronization at the instance level.
     *
     * @param timer Timer to enforce time limit
     */
    // Visible for testing
    protected void closeInternal(Timer timer) {
        // we do not need to re-enable wake-ups since we are closing already
        Utils.closeQuietly(fetchBuffer, "fetchBuffer");
        Utils.closeQuietly(decompressionBufferSupplier, "decompressionBufferSupplier");
    }

    public void close(final Timer timer) {
        idempotentCloser.close(() -> closeInternal(timer));
    }

    @Override
    public void close() {
        close(time.timer(Duration.ZERO));
    }

    /**
     * Defines the contract for handling fetch responses from brokers.
     * @param <T> Type of response, usually either {@link ClientResponse} or {@link Throwable}
     */
    @FunctionalInterface
    protected interface ResponseHandler<T> {

        /**
         * Handle the response from the given {@link Node target}
         */
        void handle(Node target, FetchSessionHandler.FetchRequestData data, T response);
    }
}
```



#### Error stacktrace:

```
scala.collection.Iterator$$anon$19.next(Iterator.scala:973)
	scala.collection.Iterator$$anon$19.next(Iterator.scala:971)
	scala.collection.mutable.MutationTracker$CheckedIterator.next(MutationTracker.scala:76)
	scala.collection.IterableOps.head(Iterable.scala:222)
	scala.collection.IterableOps.head$(Iterable.scala:222)
	scala.collection.AbstractIterable.head(Iterable.scala:935)
	dotty.tools.dotc.interactive.InteractiveDriver.run(InteractiveDriver.scala:164)
	dotty.tools.pc.CachingDriver.run(CachingDriver.scala:45)
	dotty.tools.pc.SignatureHelpProvider$.signatureHelp(SignatureHelpProvider.scala:31)
	dotty.tools.pc.ScalaPresentationCompiler.signatureHelp$$anonfun$1(ScalaPresentationCompiler.scala:435)
```
#### Short summary: 

java.util.NoSuchElementException: next on empty iterator