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

import java.nio.ByteBuffer;
import java.util.concurrent.atomic.AtomicInteger;

public class SharedMemoryProducer {
    public static final int SLOT_SIZE = 32768;
    static {
        System.loadLibrary("sharedmemory");
    }

    public static native ByteBuffer getPoolBigBuffer();
    public static native ByteBuffer allocateSharedMemoryByBuffer();
    public static native void commitSharedMemoryByBuffer(ByteBuffer content, int length);
    public static native ByteBuffer readSharedMemoryByBuffer();
    public static native ByteBuffer readSharedMemoryByIndex();
    public static native void closeSharedMemory();
    public static native void releaseSharedmemoryByBuffer(ByteBuffer content);
    public static native int allocateSharedMemoryIndex();
    public static native boolean commitSharedMemoryByIndex(int index, int length);
    public static native void releaseSharedMemoryIndex(int index);
}

