module.exports = {
  dependency: {
    platforms: {
      ios: {
        podspecPath: 'MathNotesMobileInk.podspec',
      },
      android: {
        sourceDir: 'android',
        packageImportPath: 'import com.mathnotes.mobileink.MobileInkPackage;',
        packageInstance: 'new MobileInkPackage()',
      },
    },
  },
};
