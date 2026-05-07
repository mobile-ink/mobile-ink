const path = require("path");
const { getDefaultConfig } = require("expo/metro-config");

const projectRoot = __dirname;
const packageRoot = path.resolve(projectRoot, "..");

const config = getDefaultConfig(projectRoot);

config.watchFolders = [...(config.watchFolders || []), packageRoot];
config.resolver = {
  ...config.resolver,
  extraNodeModules: {
    ...(config.resolver?.extraNodeModules || {}),
    "@mathnotes/mobile-ink": packageRoot,
    "@shopify/react-native-skia": path.resolve(
      projectRoot,
      "node_modules/@shopify/react-native-skia",
    ),
    react: path.resolve(projectRoot, "node_modules/react"),
    "react-native": path.resolve(projectRoot, "node_modules/react-native"),
  },
  nodeModulesPaths: [
    path.resolve(projectRoot, "node_modules"),
    path.resolve(packageRoot, "node_modules"),
  ],
};

module.exports = config;
