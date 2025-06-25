# OpenMP PR Summarizer

A tool for generating structured, LLM-powered summaries of GitHub pull requests relating to OpenMP projects, referencing the official OpenMP specification.

---

## Features

- Fetches PR diffs from any public (or accessible private) GitHub repository.
- For each changed file, finds the most relevant section in the OpenMP specification.
- Summarizes each patch with a large language model (Groq API, e.g., Llama 3).
- Two modes:
  - **Graphical User Interface (GUI)** via `app_gui.py`
  - **Terminal Command-Line Tool** via `app_terminal.py`
- Optionally, only summarize code files based on extension.

---

## Requirements

- Python 3.8+
- Dependencies: `requests`, `groq`, `pdfminer.six`, `python-dotenv`, `tkinter` (for GUI)
- Groq API Key ([get one here](https://console.groq.com))
- A copy of the OpenMP Specification PDF (by default: `OpenMP-API-Specification-6-0.pdf` in the working directory)

**Install dependencies:**

```sh
pip install requests groq pdfminer.six python-dotenv
```

Tkinter is included with most Python installations, but if you need to install it:

- **Ubuntu/Debian:** `sudo apt-get install python3-tk`
- **Windows/macOS:** Usually included by default.

---

## Setup

1. **Get a Groq API key** and (optionally) a GitHub token for higher API rate limits.
2. Create a `.env` file in your project directory with:

   ```
   GROQ_API_KEY=your_groq_api_key_here
   GITHUB_TOKEN=your_github_token_here
   ```

3. Place the OpenMP specification PDF as `OpenMP-API-Specification-6-0.pdf` in your working directory.
   (You can change the filename in the scripts if needed.)

---

## Running the GUI Application (`app_gui.py`)

The GUI version lets you enter a repository and PR number, view summaries in a scrollable window, and export results to file.

**Run:**

```sh
python app_gui.py
```

You will see a window where you can:
- Enter the repository (e.g., `Manoj-Kumar-BV/llvm-project`)
- Enter the pull request number
- Click "Summarize" to generate summaries
- Click "Export Summaries" to save as Markdown or text

---

## Running the Terminal/Command-Line Tool (`app_terminal.py`)

The terminal version prints summaries to the terminal and can export to a file.

**Usage:**

```sh
python app_terminal.py --repo owner/repo --pr PR_NUMBER [--only-code] [--export output.md]
```

**Examples:**

Summarize all files in a PR:

```sh
python app_terminal.py --repo Manoj-Kumar-BV/llvm-project --pr 123
```

Summarize only code files (see `CODE_EXTENSIONS` in the script):

```sh
python app_terminal.py --repo Manoj-Kumar-BV/llvm-project --pr 123 --only-code
```

Export results to a Markdown file:

```sh
python app_terminal.py --repo Manoj-Kumar-BV/llvm-project --pr 123 --export review.md
```

---

## Customizing

- To change which files are considered "code files", modify the `CODE_EXTENSIONS` tuple at the top of each script.
- By default, all files are summarized unless you use the `--only-code` flag (terminal) or enable the filter in the GUI code.

---

## Troubleshooting

- **Groq API errors:** If you hit rate or token limits, try reducing the patch size or upgrading your Groq account.
- **GitHub API errors:** For large/complex repos, use a GitHub personal access token in `.env`.
- **OpenMP Spec not found:** Make sure the PDF is named and located as specified.

---

## License

MIT License. See `LICENSE`.

---