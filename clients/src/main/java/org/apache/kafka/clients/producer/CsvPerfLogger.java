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
package org.apache.kafka.clients.producer;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;

public final class CsvPerfLogger implements AutoCloseable {
    private final FileChannel ch;
    private final ConcurrentLinkedQueue<byte[]> q = new ConcurrentLinkedQueue<>();
    private final ScheduledExecutorService exec;
    private final int sampleEveryN;
    private final AtomicLong seq = new AtomicLong();

    public CsvPerfLogger(String path, int sampleEveryN) throws IOException {
        this.ch = FileChannel.open(new File(path).toPath(),
                java.nio.file.StandardOpenOption.CREATE,
                java.nio.file.StandardOpenOption.WRITE,
                java.nio.file.StandardOpenOption.APPEND);
        this.sampleEveryN = Math.max(1, sampleEveryN);
        this.exec = Executors.newSingleThreadScheduledExecutor(r -> {
            Thread t = new Thread(r, "perf-csv-writer");
            t.setDaemon(true);
            return t;
        });
        this.exec.scheduleAtFixedRate(this::drain, 10, 10, TimeUnit.MILLISECONDS);
    }

    // T0/T1 공용 포맷: type,corr,timestamp,value2(optional)\n
    public void logT0ms(int corr, long t0ms) { enqueue("T0MS", corr, t0ms, null); }
    public void logT1ms(int corr, long t1ms) { enqueue("T1MS", corr, t1ms, null); }

    public void logT0ns(int corr, long t0ns) { enqueue("T0NS", corr, t0ns, null); }
    public void logT1ns(int corr, long t1ns) { enqueue("T1NS", corr, t1ns, null); }

    // 필요하면 추가 필드(예: partition/offset)도 value2로
    public void logExtra(String type, int corr, long v1, Long v2) { enqueue(type, corr, v1, v2); }

    private void enqueue(String type, int corr, long v1, Long v2) {
        if ((seq.incrementAndGet() % sampleEveryN) != 0) return; // 샘플링
        // 간단한 CSV 직렬화 (GC 압박 줄이려면 ThreadLocal StringBuilder로 교체 가능)
        String line = (v2 == null)
                ? type + "," + corr + "," + v1 + "\n"
                : type + "," + corr + "," + v1 + "," + v2 + "\n";
        q.offer(line.getBytes(StandardCharsets.US_ASCII));
    }

    private void drain() {
        try {
            ByteBuffer buf;
            byte[] a;
            while ((a = q.poll()) != null) {
                buf = ByteBuffer.wrap(a);
                while (buf.hasRemaining()) ch.write(buf);
            }
            ch.force(false);
        } catch (IOException ignore) {
            // 성능 로깅은 진단용. 실패는 무시(원하면 카운터만 올려라).
        }
    }

    @Override public void close() throws IOException {
        exec.shutdown();
        try { exec.awaitTermination(200, TimeUnit.MILLISECONDS); } catch (InterruptedException ignored) {}
        drain();
        ch.close();
    }
}
