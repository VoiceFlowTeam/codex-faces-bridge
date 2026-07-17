import type { Metadata } from "next";
import { Geist, Geist_Mono } from "next/font/google";
import { headers } from "next/headers";
import "./globals.css";

const geistSans = Geist({
  variable: "--font-geist-sans",
  subsets: ["latin"],
});

const geistMono = Geist_Mono({
  variable: "--font-geist-mono",
  subsets: ["latin"],
});

export async function generateMetadata(): Promise<Metadata> {
  const requestHeaders = await headers();
  const host = requestHeaders.get("x-forwarded-host") ?? requestHeaders.get("host") ?? "localhost:3000";
  const protocol = requestHeaders.get("x-forwarded-proto") ?? (host.startsWith("localhost") ? "http" : "https");
  const metadataBase = new URL(`${protocol}://${host}`);

  return {
    metadataBase,
    title: "Codex Micro — Interactive Web Simulator",
    description: "A tactile, browser-based Codex Micro interaction simulator.",
    icons: {
      icon: "/og.png",
      shortcut: "/og.png",
    },
    openGraph: {
      title: "Codex Micro — Interactive Web Simulator",
      description: "Turn, press, activate, and explore a tactile Codex Micro in the browser.",
      type: "website",
      images: [{ url: "/og.png", width: 1792, height: 1024, alt: "Codex Micro interactive web simulator" }],
    },
    twitter: {
      card: "summary_large_image",
      title: "Codex Micro — Interactive Web Simulator",
      description: "A tactile Codex Micro interaction simulator for the web.",
      images: ["/og.png"],
    },
  };
}

export default function RootLayout({ children }: Readonly<{ children: React.ReactNode }>) {
  return (
    <html lang="en">
      <body className={`${geistSans.variable} ${geistMono.variable}`}>{children}</body>
    </html>
  );
}
