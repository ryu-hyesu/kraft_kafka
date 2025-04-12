file://<WORKSPACE>/clients/src/main/java/org/apache/kafka/clients/consumer/SharedMemoryConsumer.java
### java.util.NoSuchElementException: next on empty iterator

occurred in the presentation compiler.

presentation compiler configuration:


action parameters:
offset: 114
uri: file://<WORKSPACE>/clients/src/main/java/org/apache/kafka/clients/consumer/SharedMemoryConsumer.java
text:
```scala
/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements. See th@@e NOTICE file distributed with
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

public class SharedMemoryConsumer {

    static {
        System.loadLibrary("sharedmemory");
    }

    public static void writeSharedMemoryByBuffer(
            String topic,
            int partition,
            byte[] recordsBytes,
            long nextOffset,
            long highWatermark,
            long lastStableOffset
    ) {
        byte[] topicBytes = topic.getBytes(StandardCharsets.UTF_8);
        int totalSize = 4 + topicBytes.length + 4 + 8 + 8 + 8 + 4 + recordsBytes.length;

        ByteBuffer buffer = ByteBuffer.allocateDirect(totalSize);
        buffer.putInt(topicBytes.length);
        buffer.put(topicBytes);
        buffer.putInt(partition);
        buffer.putLong(nextOffset);
        buffer.putLong(highWatermark);
        buffer.putLong(lastStableOffset);
        buffer.putInt(recordsBytes.length);
        buffer.put(recordsBytes);
        buffer.flip();

        // 실제 공유 메모리로 write
        writeSharedMemoryByBuffer(buffer, totalSize);
    }

    public static native void writeSharedMemoryByBuffer(ByteBuffer content, int length);
    public static native ByteBuffer readSharedMemoryByBuffer();
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
	dotty.tools.pc.HoverProvider$.hover(HoverProvider.scala:40)
	dotty.tools.pc.ScalaPresentationCompiler.hover$$anonfun$1(ScalaPresentationCompiler.scala:389)
```
#### Short summary: 

java.util.NoSuchElementException: next on empty iterator