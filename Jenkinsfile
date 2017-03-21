#!groovy

cocoaStashes = []
androidStashes = []

timeout(time: 1, unit: 'HOURS') {
    stage('gather-info') {
        node('docker') {
            getSourceArchive()
            stash includes: '**', name: 'core-source', useDefaultExcludes: false

            dependencies = readProperties file: 'dependencies.list'
            echo "VERSION: ${dependencies.VERSION}"

            gitTag = readGitTag()
            gitSha = sh(returnStdout: true, script: 'git rev-parse HEAD').trim().take(8)
            gitDescribeVersion = sh(returnStdout: true, script: 'git describe').trim()
            version = gitTag ? "${dependencies.VERSION}-g${gitSha}" : dependencies.VERSION

            echo "tag: ${gitTag}"
            if (gitTag == "") {
                echo "No tag given for this build"
                setBuildName(gitSha)
            } else {
                if (gitTag != "v${dependencies.VERSION}") {
                    def message = "Git tag '${gitTag}' does not match v${dependencies.VERSION}"
                    echo message
                    throw new IllegalStateException(message)
                } else {
                    echo "Building release: '${gitTag}'"
                    setBuildName("Tag ${gitTag}")
                }
            }
        }

        isPublishingRun = gitTag != ''
        echo "Publishing Run: ${isPublishingRun}"

        if (['master'].contains(env.BRANCH_NAME)) {
            // If we're on master, instruct the docker image builds to push to the
            // cache registry
            env.DOCKER_PUSH = "1"
        }
    }

    stage('check') {
        parallelExecutors = [checkLinuxRelease   : doBuildInDocker('Release'),
                             checkLinuxDebug     : doBuildInDocker('Debug'),
                             buildMacOsDebug     : doBuildMacOs('Debug'),
                             buildMacOsRelease   : doBuildMacOs('Release'),
                             buildWin32Release   : doBuildWindows('Release', false, 'win32'),
                             buildUwpWin32Release: doBuildWindows('Release', true, 'win32'),
                             buildUwpWin64Release: doBuildWindows('Release', true, 'win64'),
                             buildWin32Debug     : doBuildWindows('Debug', false, 'win32'),
                             buildUwpWin32Debug  : doBuildWindows('Debug', true, 'win32'),
                             buildUwpWin64Debug  : doBuildWindows('Debug', true, 'win64'),
                             packageGeneric      : doBuildPackage('generic', 'tar.gz'),
                             packageCentos7      : doBuildPackage('centos-7', 'rpm'),
                             packageCentos6      : doBuildPackage('centos-6', 'rpm'),
                             packageUbuntu1604   : doBuildPackage('ubuntu-1604', 'deb')
                             //buildUwpArmRelease: doBuildWindows('Release', true, 'arm')
                             //threadSanitizer: doBuildInDocker('jenkins-pipeline-thread-sanitizer')
            ]

        androidAbis = ['armeabi-v7a', 'x86', 'mips', 'x86_64', 'arm64-v8a']
        androidBuildTypes = ['Debug', 'Release']

        for (def i = 0; i < androidAbis.size(); i++) {
            def abi = androidAbis[i]
            for (def j = 0; j < androidBuildTypes.size(); j++) {
                def buildType = androidBuildTypes[j]
                parallelExecutors["android-${abi}-${buildType}"] = doAndroidBuildInDocker(abi, buildType, abi == 'armeabi-v7a' && buildType == 'Release')
            }
        }

        appleSdks = ['ios', 'tvos', 'watchos']
        appleBuildTypes = ['MinSizeDebug', 'Release']

        for (def i = 0; i < appleSdks.size(); i++) {
            def sdk = appleSdks[i]
            for (def j = 0; j < appleBuildTypes.size(); j++) {
                def buildType = appleBuildTypes[j]
                parallelExecutors["${sdk}${buildType}"] = doBuildAppleDevice(sdk, buildType)
            }
        }

        if (env.CHANGE_TARGET) {
            parallelExecutors['diffCoverage'] = buildDiffCoverage()
            parallelExecutors['performance'] = buildPerformance()
        }

        parallel parallelExecutors
    }

    stage('Aggregate') {
        parallel (
          cocoa: {
                node('docker') {
                    getArchive()
                    for (int i = 0; i < cocoaStashes.size(); i++) {
                        unstash name:cocoaStashes[i]
                    }
                    sh 'tools/build-cocoa.sh'
                    archiveArtifacts('realm-core-cocoa*.tar.xz')
                }
            },
          android: {
                node('docker') {
                    getArchive()
                    for (int i = 0; i < androidStashes.size(); i++) {
                        unstash name:androidStashes[i]
                    }
                    sh 'tools/build-android.sh'
                    archiveArtifacts('realm-core-android*.tar.gz')
                }
            }
        )
    }

    if (isPublishingRun) {
        stage('publish-packages') {
            parallel(
              generic: doPublishGeneric(),
                     centos7: doPublish('centos-7', 'rpm', 'el', 7),
                     centos6: doPublish('centos-6', 'rpm', 'el', 6),
                     ubuntu1604: doPublish('ubuntu-1604', 'deb', 'ubuntu', 'xenial'),
                     others: doPublishLocalArtifacts()
                )
        }
    }
}

def buildDockerEnv(name) {
    docker.withRegistry("https://012067661104.dkr.ecr.eu-west-1.amazonaws.com", "ecr:eu-west-1:aws-ci-user") {
        env.DOCKER_REGISTRY = '012067661104.dkr.ecr.eu-west-1.amazonaws.com'
        sh "./packaging/docker_build.sh $name ."
    }

    return docker.image(name)
}

def doBuildInDocker(String buildType) {
    def suffix = buildType == 'Release' ? '' : '-dbg'
    return {
        node('docker') {
            getArchive()

            def buildEnv = docker.build 'realm-core:snapshot'
            def environment = environment()
            withEnv(environment) {
                buildEnv.inside {
                    try {
                        sh """
                           mkdir build-dir
                           cd build-dir
                           cmake -D CMAKE_BUILD_TYPE=${buildType} -G Ninja ..
                           ninja
                           cd test
                           ./realm-tests
                        """
                    } finally {
                        collectCompilerWarnings('gcc', true)
                        recordTests("Linux-${buildType}")
                    }
                }
            }
        }
    }
}


def doAndroidBuildInDocker(String abi, String buildType, boolean runTestsInEmulator) {
    def cores = 4
    return {
        node('docker') {
            getArchive()
            def stashName = "android-${abi}-${buildType}"
            def buildDir = "build-${stashName}"

            def buildEnv = docker.build('realm-core-android:snapshot', '-f android.Dockerfile .')
            def environment = environment()
            withEnv(environment) {
                if(!runTestsInEmulator) {
                    buildEnv.inside {
                        try {
                            sh "tools/cross_compile.sh -o android -a ${abi} -t ${buildType} -v ${gitDescribeVersion}"
                            dir(buildDir) {
                                archiveArtifacts('*.tar.gz')
                            }
                            stash includes:"${buildDir}/*.tar.gz", name:stashName
                            androidStashes << stashName
                        } finally {
                            collectCompilerWarnings('gcc', true )
                        }
                    }
                } else {
                    docker.image('tracer0tong/android-emulator').withRun('-e ARCH=armeabi-v7a') { emulator ->
                        buildEnv.inside("--link ${emulator.id}:emulator") {
                            try {
                                sh "tools/cross_compile.sh -o android -a ${abi} -t ${buildType} -v ${gitDescribeVersion}"
                                dir(buildDir) {
                                    archiveArtifacts('*.tar.gz')
                                }
                                stash includes:"${buildDir}/*.tar.gz", name:stashName
                                androidStashes << stashName
                            } finally {
                                collectCompilerWarnings('gcc', true )
                            }
                            try {
                                sh '''
                                   cd $(find . -type d -maxdepth 1 -name build-android*)
                                   adb connect emulator
                                   timeout 10m adb wait-for-device
                                   adb push test/realm-tests /data/local/tmp
                                   find test -type f -name "*.json" -maxdepth 1 -exec adb push {} /data/local/tmp \\;
                                   find test -type f -name "*.realm" -maxdepth 1 -exec adb push {} /data/local/tmp \\;
                                   find test -type f -name "*.txt" -maxdepth 1 -exec adb push {} /data/local/tmp \\;
                                   adb shell \'cd /data/local/tmp; ./realm-tests || echo __ADB_FAIL__\' | tee adb.log
                                   ! grep __ADB_FAIL__ adb.log
                               '''
                            } finally {
                                sh '''
                                   mkdir -p build-dir/test
                                   cd build-dir/test
                                   adb pull /data/local/tmp/unit-test-report.xml
                                '''
                                recordTests('android')
                            }
                        }
                    }
                }
            }
        }
    }
}

def doBuildWindows(String buildType, boolean isUWP, String arch) {
    def cmakeDefinitions = isUWP ? '-DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0' : ''
    def archSuffix = ''
    if (arch == 'win64') {
        archSuffix = ' Win64'
    } else if (arch == 'arm') {
        archSuffix = ' ARM'
    }

    return {
        node('windows') {
            getArchive()

            dir('build-dir') {
                bat """
                    cmake ${cmakeDefinitions} -DREALM_BUILD_LIB_ONLY=1 -G \"Visual Studio 14 2015${archSuffix}\" -D CPACK_SYSTEM_NAME=${isUWP?'UWP':'Windows'}-${arch} -D CMAKE_BUILD_TYPE=${buildType} -D REALM_ENABLE_ENCRYPTION=OFF ..
                    cmake --build . --config ${buildType}
                    cpack -C ${buildType} -D CPACK_GENERATOR=TGZ
                """
                archiveArtifacts('*.tar.gz')
            }
        }
    }
}

def buildDiffCoverage() {
    return {
        node('docker') {
            getArchive()

            def buildEnv = buildDockerEnv('ci/realm-core:snapshot')
            def environment = environment()
            withEnv(environment) {
                buildEnv.inside {
                    sh '''
                        mkdir build-dir
                        cd build-dir
                        cmake -D CMAKE_BUILD_TYPE=Debug \
                              -D REALM_COVERAGE=ON \
                              -G Ninja ..
                        ninja
                        cd test
                        ./realm-tests
                        gcovr --filter=\'.*src/realm.*\' -x >gcovr.xml
                        mkdir coverage
                     '''
                    def coverageResults = sh(returnStdout: true, script: """
                        diff-cover build-dir/test/gcovr.xml \\
                                   --compare-branch=origin/${env.CHANGE_TARGET} \\
                                   --html-report build-dir/test/coverage/diff-coverage-report.html \\
                                   | grep Coverage: | head -n 1 > diff-coverage
                    """).trim()

                    publishHTML(target: [
                                  allowMissing         : false,
                                         alwaysLinkToLastBuild: false,
                                         keepAll              : true,
                                         reportDir            : 'build-dir/test/coverage',
                                         reportFiles          : 'diff-coverage-report.html',
                                         reportName           : 'Diff Coverage'
                                    ])

                    withCredentials([[$class: 'StringBinding', credentialsId: 'bot-github-token', variable: 'githubToken']]) {
                        sh """
                           curl -H \"Authorization: token ${env.githubToken}\" \\
                                -d '{ \"body\": \"${coverageResults}\\n\\nPlease check your coverage here: ${env.BUILD_URL}Diff_Coverage\"}' \\
                                \"https://api.github.com/repos/realm/realm-core/issues/${env.CHANGE_ID}/comments\"
                        """
                    }
                }
            }
        }
    }
}

def buildPerformance() {
  return {
    // Select docker-cph-X.  We want docker, metal (brix) and only one executor
    // (exclusive), if the machine changes also change REALM_BENCH_MACHID below
    node('docker && brix && exclusive') {
      getSourceArchive()

      def buildEnv = buildDockerEnv('ci/realm-core:snapshot')
      // REALM_BENCH_DIR tells the gen_bench_hist.sh script where to place results
      // REALM_BENCH_MACHID gives the results an id - results are organized by hardware to prevent mixing cached results with runs on different machines
      // MPLCONFIGDIR gives the python matplotlib library a config directory, otherwise it will try to make one on the user home dir which fails in docker
      buildEnv.inside {
        withEnv(["REALM_BENCH_DIR=${env.WORKSPACE}/test/bench/core-benchmarks", "REALM_BENCH_MACHID=docker-brix","MPLCONFIGDIR=${env.WORKSPACE}/test/bench/config"]) {
          withCredentials([[$class: 'FileBinding', credentialsId: 'c0cc8f9e-c3f1-4e22-b22f-6568392e26ae', variable: 's3cfg_config_file']]) {
              sh 's3cmd -c $s3cfg_config_file get s3://static.realm.io/downloads/core/core-benchmarks.zip core-benchmarks.zip'
          }
          sh 'unzip core-benchmarks.zip -d test/bench/'
          sh 'rm core-benchmarks.zip'

          sh '''
            cd test/bench
            mkdir -p core-benchmarks results
            ./gen_bench_hist.sh
            ./parse_bench_hist.py --local-html results/ core-benchmarks/
          '''
          zip dir: 'test/bench', glob: 'core-benchmarks/**/*', zipFile: 'core-benchmarks.zip'
          withCredentials([[$class: 'FileBinding', credentialsId: 'c0cc8f9e-c3f1-4e22-b22f-6568392e26ae', variable: 's3cfg_config_file']]) {
            sh 's3cmd -c $s3cfg_config_file put core-benchmarks.zip s3://static.realm.io/downloads/core/'
          }
          publishHTML(target: [allowMissing: false, alwaysLinkToLastBuild: false, keepAll: true, reportDir: 'test/bench/results', reportFiles: 'report.html', reportName: 'Performance Report'])
          withCredentials([[$class: 'StringBinding', credentialsId: 'bot-github-token', variable: 'githubToken']]) {
              sh "curl -H \"Authorization: token ${env.githubToken}\" " +
                 "-d '{ \"body\": \"Check the performance result here: ${env.BUILD_URL}Performance_Report\"}' " +
                 "\"https://api.github.com/repos/realm/realm-core/issues/${env.CHANGE_ID}/comments\""
          }
        }
      }
    }
  }
}

def doBuildMacOs(String buildType) {
    def sdk = 'macosx'
    return {
        node('macos || osx_vegas') {
            getArchive()

            try {
                dir("build-macos-${buildType}") {
                    withEnv(['DEVELOPER_DIR=/Applications/Xcode-8.2.app/Contents/Developer/']) {
                        // This is a dirty trick to work around a bug in xcode
                        // It will hang if launched on the same project (cmake trying the compiler out)
                        // in parallel.
                        retry(3) {
                            timeout(time: 2, unit: 'MINUTES') {
                                sh """
                                    rm -rf *
                                    cmake -D CMAKE_TOOLCHAIN_FILE=../tools/cmake/macos.toolchain.cmake \\
                                          -D CMAKE_BUILD_TYPE=${buildType} \\
                                          -D REALM_VERSION=${gitDescribeVersion} \\
                                          -G Xcode ..
                                """
                            }
                        }

                        sh """
                            xcodebuild -sdk macosx \\
                                       -configuration ${buildType} \\
                                       -target package \\
                                       ONLY_ACTIVE_ARCH=NO
                            """
                    }
                }
                archiveArtifacts("build-macos-${buildType}/*.tar.xz")

                stash includes:"build-macos-${buildType}/*.tar.xz", name:"macos-${buildType}"
                cocoaStashes << "macos-${buildType}"
            } finally {
                collectCompilerWarnings('clang', true)
            }
        }
    }
}

def doBuildAppleDevice(String sdk, String buildType) {
    return {
        node('macos || osx_vegas') {
            getArchive()

            try {
                withEnv(['DEVELOPER_DIR=/Applications/Xcode-8.2.app/Contents/Developer/']) {
                    retry(3) {
                        timeout(time: 15, unit: 'MINUTES') {
                            sh """
                                rm -rf build-*
                                tools/cross_compile.sh -o ${sdk} -t ${buildType} -v ${gitDescribeVersion}
                            """
                        }
                    }
                }
                archiveArtifacts("build-${sdk}-${buildType}/*.tar.xz")
                stash includes:"build-${sdk}-${buildType}/*.tar.xz", name:"cocoa-${sdk}-${buildType}"
                cocoaStashes << "cocoa-${sdk}-${buildType}"
            } finally {
                collectCompilerWarnings('clang', true)
            }
        }
    }
}

/**
 *  Wraps the test recorder by adding a tag which will make the test distinguishible
 */
def recordTests(tag) {
    def tests = readFile('build-dir/test/unit-test-report.xml')
    def modifiedTests = tests.replaceAll('realm-core-tests', tag)
    writeFile file: 'build-dir/test/modified-test-report.xml', text: modifiedTests
    junit 'build-dir/test/modified-test-report.xml'
}

def collectCompilerWarnings(compiler, fail) {
    def parserName
    if (compiler == 'gcc') {
        parserName = 'GNU Make + GNU C Compiler (gcc)'
    } else if (compiler == 'clang') {
        parserName = 'Clang (LLVM based)'
    } else if (compiler == 'msbuild') {
        parserName = 'MSBuild'
    }
    step([
           $class                 : 'WarningsPublisher',
          canComputeNew          : false,
          canRunOnFailed         : true,
          canResolveRelativePaths: false,
          consoleParsers         : [[parserName: parserName]],
          defaultEncoding        : '',
          excludePattern         : '',
          unstableTotalAll       : fail ? '0' : '',
          healthy                : '',
          includePattern         : '',
          messagesPattern        : '',
          unHealthy              : ''
             ])
}

def environment() {
    return [
        "REALM_MAX_BPNODE_SIZE_DEBUG=4",
        "UNITTEST_SHUFFLE=1",
        "UNITTEST_RANDOM_SEED=random",
        "UNITTEST_THREADS=1",
        "UNITTEST_XML=1"
        ]
}

def readGitTag() {
    def command = 'git describe --exact-match --tags HEAD'
    def returnStatus = sh(returnStatus: true, script: command)
    if (returnStatus == 0) {
        def tag = sh(returnStdout: true, script: command).trim()
        return tag
    } else {
        return ''
    }
}

def doBuildPackage(distribution, fileType) {
    return {
        node('docker') {
            getSourceArchive()

            docker.withRegistry("https://012067661104.dkr.ecr.eu-west-1.amazonaws.com", "ecr:eu-west-1:aws-ci-user") {
                env.DOCKER_REGISTRY = '012067661104.dkr.ecr.eu-west-1.amazonaws.com'
                withCredentials([[$class: 'StringBinding', credentialsId: 'packagecloud-sync-devel-master-token', variable: 'PACKAGECLOUD_MASTER_TOKEN']]) {
                    sh "sh packaging/package.sh ${distribution}"
                }
            }

            dir('packaging/out') {
                archiveArtifacts artifacts: "${distribution}/*.${fileType}"
                stash includes: "${distribution}/*.${fileType}", name: "packages-${distribution}"
            }
        }
    }
}

def doPublish(distribution, fileType, distroName, distroVersion) {
    return {
        node {
            getSourceArchive()
            packaging = load './packaging/publish.groovy'

            dir('packaging/out') {
                unstash "packages-${distribution}"
                dir(distribution) {
                    packaging.uploadPackages('sync-devel', fileType, distroName, distroVersion, "*.${fileType}")
                }
            }
        }
    }
}

def doPublishGeneric() {
    return {
        node {
            getSourceArchive()
            def version = getVersion()
            def topdir = pwd()
            dir('packaging/out') {
                unstash "packages-generic"
            }
            dir("core/v${version}/linux") {
                sh "mv ${topdir}/packaging/out/generic/realm-core-*.tgz ./realm-core-${version}.tgz"
            }

            step([
                   $class                              : 'S3BucketPublisher',
                  dontWaitForConcurrentBuildCompletion: false,
                  entries                             : [[
                           bucket                 : 'realm-ci-artifacts',
                                                          excludedFile           : '',
                                                          flatten                : false,
                                                          gzipFiles              : false,
                                                          managedArtifacts       : false,
                                                          noUploadOnFailure      : true,
                                                          selectedRegion         : 'us-east-1',
                                                          sourceFile             : "core/v${version}/linux/*.tgz",
                                                          storageClass           : 'STANDARD',
                                                          uploadFromSlave        : false,
                                                          useServerSideEncryption: false
                             ]],
                  profileName                         : 'hub-jenkins-user',
                  userMetadata                        : []
                     ])
        }
    }
}

def doPublishLocalArtifacts() {
    // TODO create a Dockerfile for an image only containing s3cmd
    return {
        node('aws') {
            deleteDir()
            unstash 'cocoa-package'
            unstash 'node-linux-package'
            unstash 'node-cocoa-package'
            unstash 'android-package'
            unstash 'dylib-osx-package'
            unstash 'windows-package'

            withCredentials([[$class: 'FileBinding', credentialsId: 'c0cc8f9e-c3f1-4e22-b22f-6568392e26ae', variable: 's3cfg_config_file']]) {
                sh 'find . -type f -name "*.tar.*" -maxdepth 1 -exec s3cmd -c $s3cfg_config_file put {} s3://static.realm.io/downloads/core/ \\;'
                sh 'find . -type f -name "*.zip" -maxdepth 1 -exec s3cmd -c $s3cfg_config_file put {} s3://static.realm.io/downloads/core/ \\;'
            }
        }
    }
}

def setBuildName(newBuildName) {
    currentBuild.displayName = "${currentBuild.displayName} - ${newBuildName}"
}

def getArchive() {
    deleteDir()
    unstash 'core-source'
}

def getSourceArchive() {
    checkout(
        [
          $class           : 'GitSCM',
          branches         : scm.branches,
          gitTool          : 'native git',
          extensions       : scm.extensions + [[$class: 'CleanCheckout']],
          userRemoteConfigs: scm.userRemoteConfigs
        ]
    )
}
