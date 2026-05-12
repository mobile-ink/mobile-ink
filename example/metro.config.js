const path = require("path");
const { getDefaultConfig } = require("expo/metro-config");

const projectRoot = __dirname;
const packageRoot = path.resolve(projectRoot, "..");
const appNodeModules = path.resolve(projectRoot, "node_modules");

const peerDependencyAliases = [
  "@shopify/react-native-skia",
  "react",
  "react-native",
  "react-native-gesture-handler",
  "react-native-reanimated",
  "react-native-worklets",
];
const escapedPackageRoot = packageRoot.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
const blockedRootPeerDependencies = peerDependencyAliases.map(
  (packageName) =>
    new RegExp(
      `${escapedPackageRoot}/node_modules/${packageName.replace(
        "/",
        "\\/",
      )}(/.*)?$`,
    ),
);

const config = getDefaultConfig(projectRoot);
const existingBlockList = config.resolver?.blockList
  ? Array.isArray(config.resolver.blockList)
    ? config.resolver.blockList
    : [config.resolver.blockList]
  : [];

config.watchFolders = [...(config.watchFolders || []), packageRoot];
config.resolver = {
  ...config.resolver,
  blockList: [...existingBlockList, ...blockedRootPeerDependencies],
  extraNodeModules: {
    ...(config.resolver?.extraNodeModules || {}),
    "@mathnotes/mobile-ink": packageRoot,
    ...Object.fromEntries(
      peerDependencyAliases.map((packageName) => [
        packageName,
        path.resolve(appNodeModules, packageName),
      ]),
    ),
  },
  nodeModulesPaths: [appNodeModules],
};

module.exports = config;
