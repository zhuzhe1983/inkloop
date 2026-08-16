import InkStudio from "./ink-studio";
import { I18nProvider } from "./lib/i18n";

export default function Home() {
  return (
    <I18nProvider>
      <InkStudio />
    </I18nProvider>
  );
}
