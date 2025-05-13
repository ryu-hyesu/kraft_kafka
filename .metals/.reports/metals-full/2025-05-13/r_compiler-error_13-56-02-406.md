file://<WORKSPACE>/clients/src/main/java/org/apache/kafka/clients/consumer/SharedMemoryConsumer.java
### java.util.NoSuchElementException: next on empty iterator

occurred in the presentation compiler.

presentation compiler configuration:


action parameters:
uri: file://<WORKSPACE>/clients/src/main/java/org/apache/kafka/clients/consumer/SharedMemoryConsumer.java
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

 package org.apache.kafka.clients.consumer;

 import java.nio.ByteBuffer;
 import java.nio.charset.StandardCharsets;
 import java.util.List;
 import java.util.Map;
 import java.util.Optional;

 import org.apache.kafka.common.TopicPartition;
import org.apache.kafka.common.compress.Compression;
 import org.apache.kafka.common.header.internals.RecordHeaders;
 import org.apache.kafka.common.message.FetchResponseData;
import org.apache.kafka.common.record.MemoryRecords;
import org.apache.kafka.common.record.MemoryRecordsBuilder;
import org.apache.kafka.common.record.TimestampType;
import org.apache.kafka.common.serialization.Deserializer;

 public class SharedMemoryConsumer {
 
    static {
         System.loadLibrary("sharedmemory");
     }
 
     public static class SharedMemoryMessage {
         public final String topic;
         public final int partition;
         public final ByteBuffer recordsBytes;
         public final long nextOffset;
         public final long highWatermark;
         public final long lastStableOffset;
 
         public SharedMemoryMessage(String topic,
                                    int partition,
                                    ByteBuffer recordsBytes,
                                    long nextOffset,
                                    long highWatermark,
                                    long lastStableOffset) {
             this.topic = topic;
             this.partition = partition;
             this.recordsBytes = recordsBytes;
             this.nextOffset = nextOffset;
             this.highWatermark = highWatermark;
             this.lastStableOffset = lastStableOffset;
         }
     }
 
    public static void writeSharedMemoryByBuffer(
        String topic,
        int partition,
        byte[] key,
        byte[] value,
        long timestamp,
        long nextOffset,
        long highWatermark,
        long lastStableOffset
    ) {
        byte[] topicBytes = topic.getBytes(StandardCharsets.UTF_8);
        int keyLen = key != null ? key.length : -1;
        int valueLen = value != null ? value.length : -1;

        int recordBodySize = 4 + (keyLen > 0 ? keyLen : 0) +
                            4 + (valueLen > 0 ? valueLen : 0) +
                            8;

        int totalSize =
                4 +                     // dump
                4 + topicBytes.length + // topic length + topic
                4 +                     // partition
                8 +                     // nextOffset
                8 +                     // highWatermark
                8 +                     // lastStableOffset
                4 + recordBodySize;     // record length + record data

        ByteBuffer buffer = ByteBuffer.allocateDirect(totalSize);

        // dump
        buffer.putInt(0);

        // topic
        buffer.putInt(topicBytes.length);
        buffer.put(topicBytes);

        // metadata
        buffer.putInt(partition);
        buffer.putLong(nextOffset);
        buffer.putLong(highWatermark);
        buffer.putLong(lastStableOffset);

        // record content
        buffer.putInt(recordBodySize);  // record content length

        buffer.putInt(keyLen);
        if (keyLen > 0) buffer.put(key);

        buffer.putInt(valueLen);
        if (valueLen > 0) buffer.put(value);

        buffer.putLong(timestamp);

        buffer.flip();
        writeSharedMemoryByBuffer(buffer, totalSize);
    }

     public static <K, V> ConsumerRecords<K, V> readSharedMemoryBySharedMessage(
             Deserializer<K> keyDeserializer,
             Deserializer<V> valueDeserializer
     ) {
         ByteBuffer buffer = readSharedMemoryByBuffer();
         
         if (buffer == null || buffer.remaining() < 32) return ConsumerRecords.empty();
         
         buffer.rewind();
 
         int topicLen = buffer.getInt();
         byte[] topicBytes = new byte[topicLen];
         buffer.get(topicBytes);
         String topic = new String(topicBytes, StandardCharsets.UTF_8);
 
         int partition = buffer.getInt();
         long nextOffset = buffer.getLong();
         long highWatermark = buffer.getLong(); // unused but parsed
         long lastStableOffset = buffer.getLong(); // unused but parsed
 
         int recordsLen = buffer.getInt();
         ByteBuffer recordsBuf = buffer.slice();
         recordsBuf.limit(recordsLen);
         recordsBuf.rewind();
 
         // FORMAT: [keyLen][keyBytes][valueLen][valueBytes][timestamp]
         int keyLen = recordsBuf.getInt();
         byte[] keyBytes = keyLen >= 0 ? new byte[keyLen] : null;
         if (keyLen > 0) recordsBuf.get(keyBytes);
 
         int valueLen = recordsBuf.getInt();
         byte[] valueBytes = valueLen >= 0 ? new byte[valueLen] : null;
         if (valueLen > 0) recordsBuf.get(valueBytes);
 
         long timestamp = recordsBuf.getLong();
 
         K key = keyBytes != null ? keyDeserializer.deserialize(topic, keyBytes) : null;
         V value = valueBytes != null ? valueDeserializer.deserialize(topic, valueBytes) : null;
        
         ConsumerRecord<K, V> record = new ConsumerRecord<>(
                 topic,
                 partition,
                 nextOffset - 1,
                 timestamp,
                 TimestampType.CREATE_TIME,
                 keyLen,
                 valueLen,
                 key,
                 value,
                 new RecordHeaders(),
                 Optional.empty(),
                 Optional.empty()
         );

         TopicPartition tp = new TopicPartition(topic, partition);
 
         return new ConsumerRecords<>(Map.of(tp, List.of(record)), Map.of(
                 tp, new OffsetAndMetadata(nextOffset)
         ));
     }

     public static class PartitionFetchResult {
        public final String topic;
        public final int partition;
        public final FetchResponseData.PartitionData data;
    
        public PartitionFetchResult(String topic, int partition, FetchResponseData.PartitionData data) {
            this.topic = topic;
            this.partition = partition;
            this.data = data;
        }
    }
    
     public static PartitionFetchResult readSharedMemoryAsMemoryRecords() {
        ByteBuffer buffer = readSharedMemoryByBuffer();
        
        if (buffer == null || buffer.remaining() < 32) return new PartitionFetchResult(null, -1, null);
        
        buffer.rewind();
    
        int topicLen = buffer.getInt();
        byte[] topicBytes = new byte[topicLen];
        buffer.get(topicBytes);
        String topic = new String(topicBytes, StandardCharsets.UTF_8);
    
        int partition = buffer.getInt();
        long nextOffset = buffer.getLong();
        long highWatermark = buffer.getLong(); // optional
        long lastStableOffset = buffer.getLong(); // optional
    
        int recordsLen = buffer.getInt();
        ByteBuffer recordsBuf = buffer.slice();
        recordsBuf.limit(recordsLen);
        recordsBuf.rewind();
    
        int keyLen = recordsBuf.getInt();
        byte[] keyBytes = keyLen >= 0 ? new byte[keyLen] : null;
        if (keyLen > 0) recordsBuf.get(keyBytes);
    
        int valueLen = recordsBuf.getInt();
        byte[] valueBytes = valueLen >= 0 ? new byte[valueLen] : null;
        if (valueLen > 0) recordsBuf.get(valueBytes);
    
        long timestamp = recordsBuf.getLong();
    
        long baseOffset = nextOffset - 1;
        ByteBuffer outBuffer = ByteBuffer.allocate(512);
    
        // 빈 버퍼(512) 할당
        MemoryRecordsBuilder builder = MemoryRecords.builder(
            outBuffer,
            Compression.NONE,
            TimestampType.CREATE_TIME,
            baseOffset
        );

        // Kafak header | CRC | RecordBatch | Record 포맷으로 채워짐
        // ㄸ
        builder.append(timestamp, keyBytes, valueBytes); 
        builder.close();

        MemoryRecords records = builder.build();

        // MemoryRecords.readableRecords(buffer); // memoryRecord 형식이 아니라면 절대 해서 안 됨 파싱 안 됨!
    
        FetchResponseData.PartitionData pd = new FetchResponseData.PartitionData();
        pd.setPartitionIndex(partition);
        pd.setErrorCode((short) 0); // Errors.NONE.code()
        pd.setHighWatermark(highWatermark);
        pd.setLastStableOffset(lastStableOffset);
        pd.setLogStartOffset(-1); // optional
        pd.setRecords(records);
        
        return new PartitionFetchResult(topic, partition, pd);
    }
    
     public static native void writeSharedMemoryToServer(ByteBuffer content, int length);
     public static native void writeSharedMemoryByBuffer(ByteBuffer content, int length);
     public static native ByteBuffer readSharedMemoryByBuffer();
     public static native ByteBuffer readSharedMemoryByConsumer();

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
	dotty.tools.pc.WithCompilationUnit.<init>(WithCompilationUnit.scala:31)
	dotty.tools.pc.SimpleCollector.<init>(PcCollector.scala:351)
	dotty.tools.pc.PcSemanticTokensProvider$Collector$.<init>(PcSemanticTokensProvider.scala:63)
	dotty.tools.pc.PcSemanticTokensProvider.Collector$lzyINIT1(PcSemanticTokensProvider.scala:63)
	dotty.tools.pc.PcSemanticTokensProvider.Collector(PcSemanticTokensProvider.scala:63)
	dotty.tools.pc.PcSemanticTokensProvider.provide(PcSemanticTokensProvider.scala:88)
	dotty.tools.pc.ScalaPresentationCompiler.semanticTokens$$anonfun$1(ScalaPresentationCompiler.scala:111)
```
#### Short summary: 

java.util.NoSuchElementException: next on empty iterator