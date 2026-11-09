// Optional asset maintenance tool; the desktop build uses the checked-in PNGs.
const fs = require("node:fs");
const path = require("node:path");
const {chromium} = require(process.argv[2] || "playwright");
const directory = path.resolve(__dirname, "../assets/icons");

async function main()
{
    const edge = "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe";
    const executablePath = process.env.BROWSER_EXECUTABLE || (fs.existsSync(edge) ? edge : undefined);
    const browser = await chromium.launch({headless: true, executablePath});
    try
    {
        for (const name of fs.readdirSync(directory).filter(name => name.endsWith(".svg")))
        {
            const original = name.replace("-midnight.svg", ".svg");
            const tree = original.startsWith("voice-") || original === "channel.svg";
            const size = tree ? 24 : 36;
            const glyph = tree ? 20 : original === "brand.svg" ? 30 : 22;
            const page = await browser.newPage({viewport: {width: size, height: size}, deviceScaleFactor: 4});
            await page.setContent(`<style>
                html, body
                {
                    margin: 0;
                    width: 100%;
                    height: 100%;
                }
                body
                {
                    display: flex;
                    align-items: center;
                    justify-content: center;
                }
                svg
                {
                    width: ${glyph}px;
                    height: ${glyph}px;
                }
                </style>${fs.readFileSync(path.join(directory, name), "utf8")}`);
            await page.screenshot({path: path.join(directory, name.replace(/\.svg$/, ".png")), omitBackground: true});
            await page.close();
            console.log(name);
        }
    }
    finally
    {
        await browser.close();
    }
}

main().catch(error =>
{
    console.error(error.message);
    process.exitCode = 1;
});
