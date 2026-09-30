include(ExternalProject)

ExternalProject_Add(pt_download
    GIT_REPOSITORY https://github.com/decentralised-art/PT.git
    GIT_TAG main
    PREFIX          "${PT_REPO_PREFIX}"
    SOURCE_DIR      "${PT_REPO_PREFIX}/src/pt_repo"
    SOURCE_SUBDIR   "solidity"
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    LOG_DOWNLOAD ON
    EXCLUDE_FROM_ALL TRUE
)

set(PT_REPO_PREFIX "${PT_REPO_PREFIX}/src/pt_repo")
