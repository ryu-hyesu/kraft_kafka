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

 import java.io.File;
 import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.StandardOpenOption;
import java.util.List;
import java.util.Map;
 import java.util.Optional;

 import org.apache.kafka.common.TopicPartition;
import org.apache.kafka.common.header.internals.RecordHeaders;
 import org.apache.kafka.common.message.FetchResponseData;
 import org.apache.kafka.common.message.FetchResponseData.AbortedTransaction;
import org.apache.kafka.common.record.FileRecords;
import org.apache.kafka.common.record.MemoryRecords;
import org.apache.kafka.common.record.Records;
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

    public static void buildSharedMemoryResponseBuffer(
            String topic,
            int partition,
            short errorCode,
            long highWatermark,
            long lastStableOffset,
            long logStartOffset,
            int preferredReadReplica,
            List<AbortedTransaction> abortedTxns,
            Records records
    ) throws Exception {
        byte[] topicBytes = topic.getBytes(StandardCharsets.UTF_8);
        ByteBuffer recordBuffer;
        if (records instanceof FileRecords) {
            
            FileRecords fr = (FileRecords) records;
            File file = fr.file();

            long readOffset = fr.channel().position() - fr.sizeInBytes(); // 추정 시작점
            int size = fr.sizeInBytes();

            try (FileChannel readChannel = FileChannel.open(file.toPath(), StandardOpenOption.READ)) {
                recordBuffer = ByteBuffer.allocate(size);
                readChannel.read(recordBuffer, readOffset);
                recordBuffer.flip(); // position=0, limit=actual size
            
                // dumpBuffer(recordBuffer, 512);
            }

        } else if (records instanceof MemoryRecords) {
            recordBuffer = ((MemoryRecords) records).buffer().duplicate();
            recordBuffer.rewind(); // 반드시 rewind
            recordBuffer.limit(recordBuffer.capacity()); // 명확히 limit 지정
        } else {
            throw new IllegalArgumentException("Unknown records type: " + records.getClass());
        }

        // 이제 safety check
        int topicLen = topicBytes.length;
        int abortedCount = abortedTxns != null ? abortedTxns.size() : 0;
        int recordLen = recordBuffer.remaining();

        if (recordLen < 0 || recordLen > 10 * 1024 * 1024) {
            throw new IllegalStateException("recordLen is abnormal: " + recordLen);
        }


        // 전체 크기 계산
        int totalSize =
                4 + // dump
                4 + topicLen +          // topic length + topic
                4 +                     // partition
                2 +                     // errorCode
                8 + 8 + 8 +             // highWatermark, lastStableOffset, logStartOffset
                4 +                     // preferredReadReplica
                4 + abortedCount * 16 + // abortedTxns (count + each 8+8 bytes)
                4 + recordLen;          // recordBuffer length + data

        ByteBuffer buffer = ByteBuffer.allocateDirect(totalSize);

        buffer.putInt(0);
        buffer.putInt(topicLen);
        buffer.put(topicBytes);

        buffer.putInt(partition);
        buffer.putShort(errorCode);
        buffer.putLong(highWatermark);
        buffer.putLong(lastStableOffset);
        buffer.putLong(logStartOffset);
        buffer.putInt(preferredReadReplica);

        buffer.putInt(abortedCount);
        if (abortedTxns != null) {
            for (AbortedTransaction txn : abortedTxns) {
                buffer.putLong(txn.producerId());
                buffer.putLong(txn.firstOffset());
            }
        }

        buffer.putInt(recordLen);
        buffer.put(recordBuffer);

        buffer.flip();


        // dumpBuffer(buffer, 512);
        // completed.rewind(); // 위치 초기화
        writeSharedMemoryByBuffer(buffer, totalSize);
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

    public static void dumpBuffer(ByteBuffer buffer, int maxBytes) {
        int limit = Math.min(buffer.remaining(), maxBytes);
        byte[] data = new byte[limit];
        
        buffer.mark(); // 현재 position 저장
        buffer.get(data); // 읽어옴
        buffer.reset(); // position 복원

        System.out.println("✅ before write in broker");
        for (int i = 0; i < data.length; i++) {
            if (i % 16 == 0) System.out.printf("%04X: ", i);
            System.out.printf("%02X ", data[i]);
            if ((i + 1) % 16 == 0) System.out.println();
        }
        if (data.length % 16 != 0) System.out.println(); // 줄 끝맞춤
    }

    public static void dumpBuffer2(ByteBuffer buffer, int maxBytes) {
        int limit = Math.min(buffer.remaining(), maxBytes);
        byte[] data = new byte[limit];
        
        buffer.mark(); // 현재 position 저장
        buffer.get(data); // 읽어옴
        buffer.reset(); // position 복원

        System.out.println("🛠 after read in consumer");
        for (int i = 0; i < data.length; i++) {
            if (i % 16 == 0) System.out.printf("%04X: ", i);
            System.out.printf("%02X ", data[i]);
            if ((i + 1) % 16 == 0) System.out.println();
        }
        if (data.length % 16 != 0) System.out.println(); // 줄 끝맞춤
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

        // dumpBuffer2(buffer, 200); // 직접 구현한 16진수 버퍼 디버깅 함수 있으면 사용

        int topicLen = buffer.getInt();
        byte[] topicBytes = new byte[topicLen];
        buffer.get(topicBytes);
        String topic = new String(topicBytes, StandardCharsets.UTF_8);

        int partition = buffer.getInt();
        short errorCode = buffer.getShort();
        long highWatermark = buffer.getLong();
        long lastStableOffset = buffer.getLong();
        long logStartOffset = buffer.getLong();
        int preferredReadReplica = buffer.getInt();
        int abortedCount = buffer.getInt();
        int recordLen = buffer.getInt();

        int recordStartPos = buffer.position(); // position after recordLen
        // System.err.printf("recordStartPos=%d, recordLen=%d, buffer.limit()=%d\n",
            // recordStartPos, recordLen, buffer.limit());

        
        MemoryRecords records;
        if (recordLen <= 0 || buffer.remaining() < recordLen) {
            records = MemoryRecords.EMPTY;
        } else {
            ByteBuffer recordSlice = buffer.duplicate();
            
            recordSlice.position(recordStartPos);
            recordSlice.limit(recordStartPos + recordLen);
            recordSlice = recordSlice.slice(); // ✔ this gives position=0, limit=recordLen

            debugRecordSlice(recordSlice, 128);
            // System.err.printf("get(0) = 0x%02X, get(1) = 0x%02X\n", recordSlice.get(0), recordSlice.get(1));

            if (recordLen > 10_000_000) {
                System.err.printf("[SHM] Suspicious recordLen=%d → skipping record\n", recordLen);
                records = MemoryRecords.EMPTY;
            } else {
                records = MemoryRecords.readableRecords(recordSlice); // ✅ actual parsing
            }
        }

        FetchResponseData.PartitionData pd = new FetchResponseData.PartitionData();
        pd.setPartitionIndex(partition);
        pd.setErrorCode((short) 0); // Errors.NONE.code()
        pd.setHighWatermark(highWatermark);
        pd.setLastStableOffset(lastStableOffset);
        pd.setLogStartOffset(logStartOffset); // optional
        pd.setPreferredReadReplica(preferredReadReplica);
        pd.setRecords(records);
        
        return new PartitionFetchResult(topic, partition, pd);
    }

    private static void debugRecordSlice(ByteBuffer recordSlice, int maxBytes) {
        ByteBuffer copy = recordSlice.duplicate(); // 포지션 영향 안주게 복제
        
        int len = Math.min(copy.remaining(), maxBytes);
        byte[] data = new byte[len];
        copy.get(data);
        
        System.err.printf("🧪 [SHM] Dumping %d bytes before parsing:\n", len);
        for (int i = 0; i < len; i++) {
            if (i % 16 == 0) System.err.printf("%04X: ", i);
            System.err.printf("%02X ", data[i]);
            if ((i + 1) % 16 == 0) System.err.println();
        }
        if (len % 16 != 0) System.err.println();
    }

    
     public static native void writeSharedMemoryToServer(ByteBuffer content, int length);
     public static native void writeSharedMemoryByBuffer(ByteBuffer content, int length);
     public static native ByteBuffer readSharedMemoryByBuffer();
     public static native ByteBuffer readSharedMemoryByConsumer();

 }