error id: file://<WORKSPACE>/clients/src/main/java/org/apache/kafka/clients/consumer/internals/ClassicKafkaConsumer.java
file://<WORKSPACE>/clients/src/main/java/org/apache/kafka/clients/consumer/internals/ClassicKafkaConsumer.java
### com.thoughtworks.qdox.parser.ParseException: syntax error @[1,1]

error in qdox parser
file content:
```java
offset: 0
uri: file://<WORKSPACE>/clients/src/main/java/org/apache/kafka/clients/consumer/internals/ClassicKafkaConsumer.java
text:
```scala
@@
```

```



#### Error stacktrace:

```
com.thoughtworks.qdox.parser.impl.Parser.yyerror(Parser.java:2025)
	com.thoughtworks.qdox.parser.impl.Parser.yyparse(Parser.java:2147)
	com.thoughtworks.qdox.parser.impl.Parser.parse(Parser.java:2006)
	com.thoughtworks.qdox.library.SourceLibrary.parse(SourceLibrary.java:232)
	com.thoughtworks.qdox.library.SourceLibrary.parse(SourceLibrary.java:190)
	com.thoughtworks.qdox.library.SourceLibrary.addSource(SourceLibrary.java:94)
	com.thoughtworks.qdox.library.SourceLibrary.addSource(SourceLibrary.java:89)
	com.thoughtworks.qdox.library.SortedClassLibraryBuilder.addSource(SortedClassLibraryBuilder.java:162)
	com.thoughtworks.qdox.JavaProjectBuilder.addSource(JavaProjectBuilder.java:174)
	scala.meta.internal.mtags.JavaMtags.indexRoot(JavaMtags.scala:48)
	scala.meta.internal.mtags.MtagsIndexer.index(MtagsIndexer.scala:21)
	scala.meta.internal.mtags.MtagsIndexer.index$(MtagsIndexer.scala:20)
	scala.meta.internal.mtags.JavaMtags.index(JavaMtags.scala:38)
	scala.meta.internal.mtags.Mtags$.allToplevels(Mtags.scala:150)
	scala.meta.internal.metals.DefinitionProvider.fromMtags(DefinitionProvider.scala:359)
	scala.meta.internal.metals.DefinitionProvider.$anonfun$positionOccurrence$4(DefinitionProvider.scala:278)
	scala.Option.orElse(Option.scala:477)
	scala.meta.internal.metals.DefinitionProvider.$anonfun$positionOccurrence$1(DefinitionProvider.scala:278)
	scala.Option.flatMap(Option.scala:283)
	scala.meta.internal.metals.DefinitionProvider.positionOccurrence(DefinitionProvider.scala:270)
	scala.meta.internal.metals.DefinitionProvider.definitionFromSnapshot(DefinitionProvider.scala:322)
	scala.meta.internal.metals.DefinitionProvider.$anonfun$definition$2(DefinitionProvider.scala:113)
	scala.Option.map(Option.scala:242)
	scala.meta.internal.metals.DefinitionProvider.fromSemanticDb$1(DefinitionProvider.scala:113)
	scala.meta.internal.metals.DefinitionProvider.$anonfun$definition$6(DefinitionProvider.scala:142)
	scala.meta.internal.metals.DefinitionProvider.$anonfun$definition$15(DefinitionProvider.scala:150)
	scala.concurrent.impl.Promise$Transformation.run(Promise.scala:470)
	java.base/java.util.concurrent.ThreadPoolExecutor.runWorker(ThreadPoolExecutor.java:1136)
	java.base/java.util.concurrent.ThreadPoolExecutor$Worker.run(ThreadPoolExecutor.java:635)
	java.base/java.lang.Thread.run(Thread.java:840)
```
#### Short summary: 

QDox parse error in file://<WORKSPACE>/clients/src/main/java/org/apache/kafka/clients/consumer/internals/ClassicKafkaConsumer.java