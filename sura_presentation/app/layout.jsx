import "../src/index.css";

export const metadata = {
  metadataBase: new URL("https://suralang.site"),
  title: "Sura Language — C++로 구현한 프로그래밍 언어",
  description:
    "Sura Language 공식 사이트. Starter 프로젝트 생성, 실행, 테스트, 입문 예제와 전체 언어 레퍼런스를 제공합니다.",
  alternates: {
    canonical: "/",
  },
  icons: {
    icon: "/favicon.ico",
    shortcut: "/favicon.ico",
  },
  openGraph: {
    type: "website",
    url: "/",
    siteName: "Sura Language",
    title: "Sura Language 1.11.1 — Microsoft Store에서 설치",
    description: "SuraTeam의 무료 Windows 프로그래밍 언어. 프로젝트 생성 · 실행 · 테스트까지 한 번에 시작하세요.",
    images: ["/og.png"],
  },
  twitter: {
    card: "summary_large_image",
    title: "Sura Language 1.11.1 — Microsoft Store에서 설치",
    description: "SuraTeam의 무료 Windows 프로그래밍 언어. 프로젝트 생성 · 실행 · 테스트까지 한 번에 시작하세요.",
    images: ["/og.png"],
  },
};

export default function RootLayout({ children }) {
  return (
    <html lang="ko">
      <body>{children}</body>
    </html>
  );
}
