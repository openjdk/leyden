/*
 * Copyright (c) 2023, 2024, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

package jdk.test.lib.cds;

import java.io.File;
import jdk.test.lib.cds.CDSTestUtils;
import jdk.test.lib.process.ProcessTools;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.StringArrayUtils;

/*
 * This is a base class used for testing CDS functionalities with complex applications.
 * You can define the application by overridding the vmArgs(), classpath() and appCommandLine()
 * methods. Application-specific validation checks can be implemented with checkExecution().
 *
 * Note: to debug the new workflow, run jtreg with -vmoption:-DCDSAppTester.split.new.workflow=true
 * This will run the new workflow in two separate processes that you can rerun easily inside a debugger.
 * Also, the log files are easier to read.
*/
abstract public class CDSAppTester {
    private final String name;
    private final String classListFile;
    private final String classListFileLog;
    private final String staticArchiveFile;
    private final String staticArchiveFileLog;
    private final String dynamicArchiveFile;
    private final String dynamicArchiveFileLog;
    private final String productionRunLog;
    private final String codeCacheFile;  // old workflow
    private final String codeCacheFileLog;
    private final String cdsFile;        // new workflow: -XX:CacheDataStore=<foo>.cds
    private final String cdsFileLog;
    private final String cdsFilePreImage;        // new workflow: -XX:CacheDataStore=<foo>.cds
    private final String cdsFilePreImageLog;
    private final String aotFile;        // new workflow = cdsFile + ".code"

    public CDSAppTester(String name) {
        // Old workflow
        this.name = name;
        classListFile = name() + ".classlist";
        classListFileLog = classListFile + ".log";
        staticArchiveFile = name() + ".static.jsa";
        staticArchiveFileLog = staticArchiveFile + ".log";
        dynamicArchiveFile = name() + ".dynamic.jsa";
        dynamicArchiveFileLog = dynamicArchiveFile + ".log";
        productionRunLog = name() + ".production.log";

        codeCacheFile = name() + ".code.jsa";
        codeCacheFileLog = codeCacheFile + ".log";
        cdsFile = name() + ".cds";
        cdsFileLog = cdsFile + ".log";
        cdsFilePreImage = cdsFile + ".preimage";
        cdsFilePreImageLog = cdsFilePreImage + ".log";
        aotFile = cdsFile + ".code";
    }

    private enum Workflow {
        STATIC,        // classic -Xshare:dump workflow
        DYNAMIC,       // classic -XX:ArchiveClassesAtExit
        LEYDEN_OLD,    // The old "5 step workflow", to be phased out
        LEYDEN,        // The new "one step training workflow" -- see JDK-8320264
    }

    public enum RunMode {
        CLASSLIST,
        DUMP_STATIC,
        DUMP_DYNAMIC,
        DUMP_CODECACHE,    // LEYDEN_OLD only
        TRAINING,          // LEYDEN only
        TRAINING0,         // LEYDEN only
        TRAINING1,         // LEYDEN only
        PRODUCTION;

        public boolean isStaticDump() {
            return this == DUMP_STATIC;
        }
        public boolean isProductionRun() {
            return this == PRODUCTION;
        }
    }

    public final String name() {
        return this.name;
    }

    // optional
    public String[] vmArgs(RunMode runMode) {
        return new String[0];
    }

    // optional
    public String classpath(RunMode runMode) {
        return null;
    }

    // must override
    // main class, followed by arguments to the main class
    abstract public String[] appCommandLine(RunMode runMode);

    // optional
    public void checkExecution(OutputAnalyzer out, RunMode runMode) throws Exception {}

    private Workflow workflow;
    private boolean checkExitValue = true;

    public final void setCheckExitValue(boolean b) {
        checkExitValue = b;
    }

    public final boolean isStaticWorkflow() {
        return workflow == Workflow.STATIC;
    }

    public final boolean isDynamicWorkflow() {
        return workflow == Workflow.DYNAMIC;
    }

    public final boolean isLeydenOldWorkflow() {
        return workflow == Workflow.LEYDEN_OLD;
    }

    public final boolean isLeydenWorkflow() {
        return workflow == Workflow.LEYDEN;
    }

    private String logToFile(String logFile, String... logTags) {
        StringBuilder sb = new StringBuilder("-Xlog:");
        String prefix = "";
        for (String tag : logTags) {
            sb.append(prefix);
            sb.append(tag);
            prefix = ",";
        }
        sb.append(":file=" + logFile + "::filesize=0");
        return sb.toString();
    }

    private void listOutputFile(String file) {
        File f = new File(file);
        if (f.exists()) {
            System.out.println("[output file: " + file + " " + f.length() + " bytes]");
        } else {
            System.out.println("[output file: " + file + " does not exist]");
        }
    }

    private OutputAnalyzer executeAndCheck(String[] cmdLine, RunMode runMode, String... logFiles) throws Exception {
        ProcessBuilder pb = ProcessTools.createTestJavaProcessBuilder(cmdLine);
        Process process = pb.start();
        OutputAnalyzer output = CDSTestUtils.executeAndLog(process, runMode.toString());
        for (String logFile : logFiles) {
            listOutputFile(logFile);
        }
        if (checkExitValue) {
            output.shouldHaveExitValue(0);
        }
        CDSTestUtils.checkCommonExecExceptions(output);
        checkExecution(output, runMode);
        return output;
    }

    private OutputAnalyzer createClassList() throws Exception {
        RunMode runMode = RunMode.CLASSLIST;
        String[] cmdLine = StringArrayUtils.concat(vmArgs(runMode),
                                                   "-Xshare:off",
                                                   "-XX:DumpLoadedClassList=" + classListFile,
                                                   "-cp", classpath(runMode),
                                                   logToFile(classListFileLog,
                                                             "class+load=debug"));
        cmdLine = StringArrayUtils.concat(cmdLine, appCommandLine(runMode));
        return executeAndCheck(cmdLine, runMode, classListFile, classListFileLog);
    }

    private OutputAnalyzer dumpStaticArchive() throws Exception {
        RunMode runMode = RunMode.DUMP_STATIC;
        String[] cmdLine = StringArrayUtils.concat(vmArgs(runMode),
                                                   "-Xlog:cds",
                                                   "-Xlog:cds+heap=error",
                                                   "-Xshare:dump",
                                                   "-XX:SharedArchiveFile=" + staticArchiveFile,
                                                   "-XX:SharedClassListFile=" + classListFile,
                                                   "-cp", classpath(runMode),
                                                   logToFile(staticArchiveFileLog,
                                                             "cds=debug",
                                                             "cds+class=debug",
                                                             "cds+heap=warning",
                                                             "cds+resolve=debug"));
        if (isLeydenOldWorkflow()) {
            cmdLine = StringArrayUtils.concat(cmdLine,
                                              "-XX:+PreloadSharedClasses",
                                              "-XX:+ArchiveInvokeDynamic",
                                              "-XX:+ArchiveDynamicProxies",
                                              "-XX:+ArchiveReflectionData");
        }

        return executeAndCheck(cmdLine, runMode, staticArchiveFile, staticArchiveFileLog);
    }

    private OutputAnalyzer dumpDynamicArchive() throws Exception {
        RunMode runMode = RunMode.DUMP_DYNAMIC;
        String[] cmdLine = new String[0];
        if (isDynamicWorkflow()) {
          // "classic" dynamic archive
          cmdLine = StringArrayUtils.concat(vmArgs(runMode),
                                            "-Xlog:cds",
                                            "-XX:ArchiveClassesAtExit=" + dynamicArchiveFile,
                                            "-cp", classpath(runMode),
                                            logToFile(dynamicArchiveFileLog,
                                                      "cds=debug",
                                                      "cds+class=debug",
                                                      "cds+resolve=debug",
                                                      "class+load=debug"));
        } else {
          // Leyden "OLD" workflow step 3
          cmdLine = StringArrayUtils.concat(vmArgs(runMode),
                                            "-Xlog:cds",
                                            "-XX:ArchiveClassesAtExit=" + dynamicArchiveFile,
                                            "-XX:SharedArchiveFile=" + staticArchiveFile,
                                            "-XX:+RecordTraining",
                                            "-cp", classpath(runMode),
                                            logToFile(dynamicArchiveFileLog,
                                                      "cds=debug",
                                                      "cds+class=debug",
                                                      "cds+resolve=debug",
                                                      "class+load=debug"));
        }
        cmdLine = StringArrayUtils.concat(cmdLine, appCommandLine(runMode));
        return executeAndCheck(cmdLine, runMode, dynamicArchiveFile, dynamicArchiveFileLog);
    }


    private OutputAnalyzer dumpCodeCache() throws Exception {
        RunMode runMode = RunMode.DUMP_CODECACHE;
        String[] cmdLine = StringArrayUtils.concat(vmArgs(runMode),
                                                   "-XX:SharedArchiveFile=" + dynamicArchiveFile,
                                                   "-XX:+ReplayTraining",
                                                   "-XX:+StoreCachedCode",
                                                   "-XX:CachedCodeFile=" + codeCacheFile,
                                                   "-XX:CachedCodeMaxSize=512M",
                                                   "-cp", classpath(runMode),
                                                   logToFile(codeCacheFileLog, "scc"));

        cmdLine = StringArrayUtils.concat(cmdLine, appCommandLine(runMode));
        return executeAndCheck(cmdLine, runMode, codeCacheFile, codeCacheFileLog);
    }

    private OutputAnalyzer oldProductionRun() throws Exception {
        RunMode runMode = RunMode.PRODUCTION;
        String[] cmdLine = StringArrayUtils.concat(vmArgs(runMode),
                                                   "-XX:SharedArchiveFile=" + dynamicArchiveFile,
                                                   "-XX:+ReplayTraining",
                                                   "-XX:+LoadCachedCode",
                                                   "-XX:CachedCodeFile=" + codeCacheFile,
                                                   "-cp", classpath(runMode),
                                                   logToFile(productionRunLog, "cds", "scc*=warning"));
        cmdLine = StringArrayUtils.concat(cmdLine, appCommandLine(runMode));
        return executeAndCheck(cmdLine, runMode, productionRunLog);
    }

    private String trainingLog(String file) {
        return logToFile(file,
                         "cds=debug",
                         "cds+class=debug",
                         "cds+heap=warning",
                         "cds+resolve=debug");
    }

    // normal training workflow (main JVM process spawns child process)
    private OutputAnalyzer trainingRun() throws Exception {
        RunMode runMode = RunMode.TRAINING;
        File f = new File(cdsFile);
        f.delete();
        String[] cmdLine = StringArrayUtils.concat(vmArgs(runMode),
                                                   "-XX:+PreloadSharedClasses",
                                                   "-XX:+ArchiveInvokeDynamic",
                                                   "-XX:+ArchiveDynamicProxies",
                                                 //"-XX:+ArchiveReflectionData",
                                                   "-XX:CacheDataStore=" + cdsFile,
                                                   "-cp", classpath(runMode),
                                                   // Use PID to distinguish the logs of the training process
                                                   // and the forked final image dump process.
                                                   "-Xlog:cds::uptime,level,tags,pid",
                                                   trainingLog(cdsFileLog));
        cmdLine = StringArrayUtils.concat(cmdLine, appCommandLine(runMode));
        OutputAnalyzer out =  executeAndCheck(cmdLine, runMode, cdsFile, cdsFileLog);
        listOutputFile(cdsFile + ".log.0"); // the preimage dump
        return out;
    }

    // "split" training workflow (launch the two processes manually, for easier debugging);
    private OutputAnalyzer trainingRun0() throws Exception {
        RunMode runMode = RunMode.TRAINING0;
        File f = new File(cdsFile);
        f.delete();
        String[] cmdLine = StringArrayUtils.concat(vmArgs(runMode),
                                                   "-XX:+UnlockDiagnosticVMOptions",
                                                   "-XX:+CDSManualFinalImage",
                                                   "-XX:+PreloadSharedClasses",
                                                   "-XX:+ArchiveInvokeDynamic",
                                                   "-XX:+ArchiveDynamicProxies",
                                                 //"-XX:+ArchiveReflectionData",
                                                   "-XX:CacheDataStore=" + cdsFile,
                                                   "-cp", classpath(runMode),
                                                   trainingLog(cdsFilePreImageLog));
        cmdLine = StringArrayUtils.concat(cmdLine, appCommandLine(runMode));
        return executeAndCheck(cmdLine, runMode, cdsFilePreImage, cdsFilePreImageLog);
    }
    private OutputAnalyzer trainingRun1() throws Exception {
        RunMode runMode = RunMode.TRAINING1;
        File f = new File(cdsFile);
        f.delete();
        String[] cmdLine = StringArrayUtils.concat(vmArgs(runMode),
                                                   "-XX:+UnlockDiagnosticVMOptions",
                                                   "-XX:+PreloadSharedClasses",
                                                   "-XX:+ArchiveInvokeDynamic",
                                                   "-XX:+ArchiveDynamicProxies",
                                                 //"-XX:+ArchiveReflectionData",
                                                   "-XX:CacheDataStore=" + cdsFile,
                                                   "-XX:CDSPreimage=" + cdsFilePreImage,
                                                   "-cp", classpath(runMode),
                                                   trainingLog(cdsFileLog));
        cmdLine = StringArrayUtils.concat(cmdLine, appCommandLine(runMode));
        return executeAndCheck(cmdLine, runMode, cdsFile, aotFile, cdsFileLog);
    }

    private OutputAnalyzer productionRun() throws Exception {
        RunMode runMode = RunMode.PRODUCTION;
        String[] cmdLine = StringArrayUtils.concat(vmArgs(runMode),
                                                   "-cp", classpath(runMode),
                                                   logToFile(productionRunLog, "cds"));

        if (isStaticWorkflow()) {
            cmdLine = StringArrayUtils.concat(cmdLine, "-XX:SharedArchiveFile=" + staticArchiveFile);
        } else if (isDynamicWorkflow()) {
            cmdLine = StringArrayUtils.concat(cmdLine, "-XX:SharedArchiveFile=" + dynamicArchiveFile);
        } else {
            cmdLine = StringArrayUtils.concat(cmdLine, "-XX:CacheDataStore=" + cdsFile);
        }

        cmdLine = StringArrayUtils.concat(cmdLine, appCommandLine(runMode));
        return executeAndCheck(cmdLine, runMode, productionRunLog);
    }

    public void run(String args[]) throws Exception {
        String err = "Must have exactly one command line argument of the following: ";
        String prefix = "";
        for (Workflow wf : Workflow.values()) {
            err += prefix;
            err += wf;
            prefix = ", ";
        }
        if (args.length != 1) {
            throw new RuntimeException(err);
        } else {
            if (args[0].equals("STATIC")) {
                runStaticWorkflow();
            } else if (args[0].equals("DYNAMIC")) {
                runDynamicWorkflow();
            } else if (args[0].equals("LEYDEN_OLD")) {
                runLeydenOldWorkflow();
            } else if (args[0].equals("LEYDEN")) {
                runLeydenWorkflow(false);
            } else if (args[0].equals("LEYDEN_TRAINONLY")) {
                runLeydenWorkflow(true);
            } else {
                throw new RuntimeException(err);
            }
        }
    }

    private void runStaticWorkflow() throws Exception {
        this.workflow = Workflow.STATIC;
        createClassList();
        dumpStaticArchive();
        productionRun();
    }

    private void runDynamicWorkflow() throws Exception {
        this.workflow = Workflow.DYNAMIC;
        dumpDynamicArchive();
        productionRun();
    }

    private void runLeydenOldWorkflow() throws Exception {
        this.workflow = Workflow.LEYDEN_OLD;
        createClassList();
        dumpStaticArchive();
        dumpDynamicArchive();
        dumpCodeCache();
        oldProductionRun();
    }

    private void runLeydenWorkflow(boolean trainOnly) throws Exception {
        this.workflow = Workflow.LEYDEN;
        if (System.getProperty("CDSAppTester.split.new.workflow") != null) {
            trainingRun0();
            trainingRun1();
        } else {
            trainingRun();
        }
        if (!trainOnly) {
            productionRun();
        }
    }
}
