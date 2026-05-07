module.exports = {
  preset: 'react-native',
  testMatch: ['<rootDir>/src/**/__tests__/**/*.(test|spec).(ts|tsx)'],
  moduleFileExtensions: ['ts', 'tsx', 'js', 'jsx'],
  testEnvironment: 'node',
  transformIgnorePatterns: [
    'node_modules/(?!(react-native|@react-native|@testing-library|react-clone-referenced-element)/)',
  ],
};
